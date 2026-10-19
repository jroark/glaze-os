/* WAV parsing and PCM / IMA ADPCM decoding (see glaze/media.h). */

#include <glaze/media.h>
#include <string.h>

static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int media_audio_fmt_parse(const uint8_t *p, uint32_t len, struct media_audio_fmt *f)
{
    if (len < 16)
        return -1;
    memset(f, 0, sizeof(*f));
    f->format = (uint16_t)le16(p);
    f->channels = (uint16_t)le16(p + 2);
    f->rate = le32(p + 4);
    f->block_align = (uint16_t)le16(p + 12);
    f->bits = (uint16_t)le16(p + 14);
    if (f->format == 0xFFFE && len >= 26)     /* WAVE_FORMAT_EXTENSIBLE */
        f->format = (uint16_t)le16(p + 24);
    if (f->channels < 1 || f->channels > 2 || f->rate < 4000 || f->rate > 96000)
        return -1;
    if (f->format == MEDIA_FMT_PCM) {
        if (f->bits != 8 && f->bits != 16)
            return -1;
        f->block_align = (uint16_t)(f->channels * f->bits / 8);
        f->block_frames = 1;
        return 0;
    }
    if (f->format == MEDIA_FMT_IMA) {
        uint32_t ch = f->channels;
        if (f->bits != 4 || f->block_align <= 4 * ch || (f->block_align - 4 * ch) % (4 * ch))
            return -1;
        f->block_frames = (uint16_t)((f->block_align - 4 * ch) * 2 / ch + 1);
        return 0;
    }
    return -1;
}

int media_wav_open(media_read_fn rd, void *ctx, uint32_t file_size, struct media_wav *w)
{
    uint8_t h[12], c[8], fmt[40];
    uint32_t off = 12;
    bool have_fmt = false;

    memset(w, 0, sizeof(*w));
    if (rd(ctx, 0, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        return -1;
    while (off + 8 <= file_size) {
        if (rd(ctx, off, c, 8) != 8)
            return -1;
        uint32_t len = le32(c + 4);
        if (!memcmp(c, "fmt ", 4)) {
            uint32_t n = MIN(len, sizeof(fmt));
            if (rd(ctx, off + 8, fmt, n) != (int)n || media_audio_fmt_parse(fmt, n, &w->fmt))
                return -1;
            have_fmt = true;
        } else if (!memcmp(c, "data", 4)) {
            if (!have_fmt)
                return -1;
            w->data_off = off + 8;
            w->data_len = MIN(len, file_size - w->data_off);
            return 0;
        }
        off += 8 + len + (len & 1);
    }
    return -1;
}

uint32_t media_fmt_frames(const struct media_audio_fmt *f, uint32_t bytes)
{
    uint32_t blocks = bytes / f->block_align, rest = bytes % f->block_align;
    uint32_t n = blocks * f->block_frames;

    if (f->format == MEDIA_FMT_IMA && rest > 4u * f->channels)
        n += (rest - 4u * f->channels) * 2 / f->channels + 1;
    return n;
}

uint32_t media_fmt_offset(const struct media_audio_fmt *f, uint32_t frame)
{
    return frame / f->block_frames * f->block_align;
}

static const int16_t ima_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
};
static const int8_t ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

struct ima_state {
    int pred, index;
};

static int16_t ima_step(struct ima_state *s, int nib)
{
    /* The exact form of the spec's shift-and-add approximation, as ffmpeg's
     * encoder assumes. */
    int diff = ((2 * (nib & 7) + 1) * ima_steps[s->index]) >> 3;

    s->pred += (nib & 8) ? -diff : diff;
    if (s->pred > 32767)
        s->pred = 32767;
    else if (s->pred < -32768)
        s->pred = -32768;
    s->index += ima_index[nib];
    if (s->index < 0)
        s->index = 0;
    else if (s->index > 88)
        s->index = 88;
    return (int16_t)s->pred;
}

/* One WAV IMA block: a 4-byte header per channel (first sample, step
 * index), then 4 bytes (8 samples) per channel in turn, low nibble first. */
static int ima_block(const uint8_t *in, uint32_t len, int ch, int16_t *out)
{
    struct ima_state st[2];
    uint32_t frames;

    if (len <= 4u * ch)
        return 0;
    for (int c = 0; c < ch; c++) {
        st[c].pred = (int16_t)le16(in + 4 * c);
        st[c].index = in[4 * c + 2] > 88 ? 88 : in[4 * c + 2];
        out[c] = (int16_t)st[c].pred;
    }
    in += 4 * ch;
    len -= 4 * ch;
    frames = 1 + len / (4u * ch) * 8;
    for (uint32_t g = 0; g < len / (4u * ch); g++) {
        for (int c = 0; c < ch; c++) {
            int16_t *o = out + (1 + g * 8) * ch + c;
            for (int k = 0; k < 4; k++) {
                uint8_t b = *in++;
                o[(2 * k) * ch] = ima_step(&st[c], b & 15);
                o[(2 * k + 1) * ch] = ima_step(&st[c], b >> 4);
            }
        }
    }
    return (int)frames;
}

int media_decode(const struct media_audio_fmt *f, const uint8_t *in, uint32_t len, int16_t *out)
{
    if (f->format == MEDIA_FMT_IMA)
        return ima_block(in, MIN(len, f->block_align), f->channels, out);
    uint32_t frames = len / f->block_align;
    uint32_t n = frames * f->channels;
    if (f->bits == 8) {
        for (uint32_t i = 0; i < n; i++)
            out[i] = (int16_t)((in[i] - 128) << 8);
    } else {
        for (uint32_t i = 0; i < n; i++)
            out[i] = (int16_t)le16(in + 2 * i);
    }
    return (int)frames;
}
