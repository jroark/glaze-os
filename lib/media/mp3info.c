/* MP3 stream probing: tags, first frame, duration and seeking. Decoding
 * itself is Helix (third_party/helix-mp3). */

#include <glaze/media.h>
#include <string.h>

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static const uint16_t br_v1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
static const uint16_t br_v2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
static const uint16_t sr_v1[3] = { 44100, 48000, 32000 };

struct hdr {
    uint32_t rate, bitrate, channels, spf, len;
    int v1;
};

/* Layer III frame header (ISO 11172-3 2.4.1.3, plus MPEG-2/2.5). */
static bool parse_hdr(const uint8_t *p, struct hdr *h)
{
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0 || (p[1] & 0x06) != 0x02)
        return false;
    int ver = (p[1] >> 3) & 3;              /* 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5 */
    int bri = p[2] >> 4, sri = (p[2] >> 2) & 3, pad = (p[2] >> 1) & 1;
    if (ver == 1 || bri == 0 || bri == 15 || sri == 3)
        return false;
    h->v1 = ver == 3;
    h->rate = sr_v1[sri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
    h->bitrate = h->v1 ? br_v1[bri] : br_v2[bri];
    h->channels = (p[3] >> 6) == 3 ? 1 : 2;
    h->spf = h->v1 ? 1152 : 576;
    h->len = h->spf / 8 * h->bitrate * 1000 / h->rate + pad;
    return true;
}

/* Copies a text frame's payload as Latin-1 (UTF-16 and UTF-8 lose
 * characters above 0xFF). */
static void id3_text(const uint8_t *p, uint32_t len, char *out, uint32_t max)
{
    uint32_t n = 0;

    if (len < 1)
        return;
    int enc = p[0];
    p++, len--;
    if (enc == 1 || enc == 2) {
        bool be = enc == 2;
        if (len >= 2 && ((p[0] == 0xFE && p[1] == 0xFF) || (p[0] == 0xFF && p[1] == 0xFE))) {
            be = p[0] == 0xFE;
            p += 2, len -= 2;
        }
        for (uint32_t i = 0; i + 1 < len && n + 1 < max; i += 2) {
            uint32_t c = be ? (uint32_t)(p[i] << 8 | p[i + 1]) : (uint32_t)(p[i] | p[i + 1] << 8);
            if (!c)
                break;
            out[n++] = (char)(c < 0x100 ? c : '?');
        }
    } else {
        for (uint32_t i = 0; i < len && n + 1 < max && p[i]; i++) {
            uint32_t c = p[i];
            if (enc == 3 && c >= 0xC0 && i + 1 < len) {
                c = ((c & 0x1F) << 6) | (p[i + 1] & 0x3F);
                i++;
                while (i + 1 < len && (p[i + 1] & 0xC0) == 0x80)
                    i++, c = '?';
            }
            out[n++] = (char)(c < 0x100 ? c : '?');
        }
    }
    out[n] = 0;
}

static void id3v2(const uint8_t *t, uint32_t len, struct media_mp3_info *mi)
{
    int ver = t[3];
    uint32_t off = 10;

    while (off + 10 <= len) {
        const uint8_t *f = t + off;
        uint32_t sz = ver == 4 ? (uint32_t)(f[4] << 21 | f[5] << 14 | f[6] << 7 | f[7]) : be32(f + 4);
        if (!f[0] || sz > len - off - 10)
            break;
        if (!memcmp(f, "TIT2", 4))
            id3_text(f + 10, sz, mi->title, sizeof(mi->title));
        else if (!memcmp(f, "TPE1", 4))
            id3_text(f + 10, sz, mi->artist, sizeof(mi->artist));
        off += 10 + sz;
    }
}

static void trim(char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && s[n - 1] == ' ')
        s[--n] = 0;
}

int media_mp3_probe(media_read_fn rd, void *ctx, uint32_t file_size, struct media_mp3_info *mi)
{
    static uint8_t b[4096];
    struct hdr h, h2;
    int n;

    memset(mi, 0, sizeof(*mi));
    mi->audio_end = file_size;
    if (file_size >= 128 && rd(ctx, file_size - 128, b, 128) == 128 && !memcmp(b, "TAG", 3)) {
        mi->audio_end = file_size - 128;
        memcpy(mi->title, b + 3, 30);
        memcpy(mi->artist, b + 33, 30);
        trim(mi->title);
        trim(mi->artist);
    }
    if (rd(ctx, 0, b, 10) == 10 && !memcmp(b, "ID3", 3)) {
        uint32_t sz = (uint32_t)(b[6] << 21 | b[7] << 14 | b[8] << 7 | b[9]) + 10;
        if (b[5] & 0x10)
            sz += 10;
        mi->audio_off = sz;
        n = rd(ctx, 0, b, MIN(sz, sizeof(b)));
        if (n > 10 && b[3] >= 3 && !(b[5] & 0x40))
            id3v2(b, (uint32_t)n, mi);
    }
    /* First frame header confirmed by the one after it. */
    for (uint32_t base = mi->audio_off; base < mi->audio_end && base < mi->audio_off + 65536; ) {
        n = rd(ctx, base, b, sizeof(b));
        if (n < 8)
            return -1;
        for (int i = 0; i + 4 <= n; i++) {
            if (!parse_hdr(b + i, &h))
                continue;
            if ((uint32_t)i + h.len + 4 <= (uint32_t)n && !parse_hdr(b + i + h.len, &h2))
                continue;
            mi->audio_off = base + (uint32_t)i;
            mi->rate = h.rate;
            mi->channels = h.channels;
            mi->bitrate = h.bitrate;
            /* Xing/Info header in the side-info gap of the first frame. */
            uint32_t xo = (uint32_t)i + 4 + (h.v1 ? (h.channels == 2 ? 32 : 17) : (h.channels == 2 ? 17 : 9));
            if (xo + 120 <= (uint32_t)n && (!memcmp(b + xo, "Xing", 4) || !memcmp(b + xo, "Info", 4))) {
                uint32_t flags = be32(b + xo + 4), p = xo + 8;
                if (flags & 1) {
                    mi->xing_frames = be32(b + p);
                    p += 4;
                }
                if (flags & 2)
                    p += 4;
                if (flags & 4) {
                    memcpy(mi->toc, b + p, 100);
                    mi->has_toc = true;
                }
            }
            if (mi->xing_frames)
                mi->duration_ms = (uint32_t)((uint64_t)mi->xing_frames * h.spf * 1000 / h.rate);
            else
                mi->duration_ms = (uint32_t)((uint64_t)(mi->audio_end - mi->audio_off) * 8 / h.bitrate);
            return 0;
        }
        base += (uint32_t)n - 3;
    }
    return -1;
}

uint32_t media_mp3_seek_offset(const struct media_mp3_info *mi, uint32_t ms)
{
    uint32_t size = mi->audio_end - mi->audio_off;

    if (!mi->duration_ms || ms >= mi->duration_ms)
        return ms ? mi->audio_end : mi->audio_off;
    if (mi->has_toc) {
        /* Percent of the duration -> 1/256 of the size, interpolated. */
        uint32_t pct1000 = (uint32_t)((uint64_t)ms * 100000 / mi->duration_ms);
        uint32_t i = pct1000 / 1000, f = pct1000 % 1000;
        uint32_t a = mi->toc[i], b2 = i < 99 ? mi->toc[i + 1] : 256;
        uint32_t x = a * 1000 + (b2 - a) * f;
        return mi->audio_off + (uint32_t)((uint64_t)x * size / 256000);
    }
    return mi->audio_off + (uint32_t)((uint64_t)ms * mi->bitrate / 8);
}
