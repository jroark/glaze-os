/* AVI (RIFF 'AVI ') headers and index: one video stream, optionally one
 * audio stream. OpenDML super-indexes are ignored; files ffmpeg writes
 * under 1 GiB also carry idx1. */

#include <glaze/media.h>
#include <stdlib.h>
#include <string.h>

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

struct avi_parse {
    media_read_fn rd;
    void *ctx;
    int vstream, astream;           /* stream numbers, or -1 */
    int nstreams;
    uint32_t movi;                  /* offset of the 'movi' fourcc */
    uint32_t movi_end;
};

static bool stream_id(const uint8_t *id, int *num)
{
    if (id[0] < '0' || id[0] > '9' || id[1] < '0' || id[1] > '9')
        return false;
    *num = (id[0] - '0') * 10 + (id[1] - '0');
    return true;
}

static int add_chunk(struct media_avi *a, struct avi_parse *ps, const uint8_t *id,
                     uint32_t off, uint32_t len, uint32_t *vcap, uint32_t *acap)
{
    int s;

    if (!stream_id(id, &s))
        return 0;
    if (s == ps->vstream && id[2] == 'd') {
        if (a->nvideo == *vcap) {
            *vcap = *vcap ? *vcap * 2 : 256;
            void *n = realloc(a->video, *vcap * sizeof(*a->video));
            if (!n)
                return -1;
            a->video = n;
        }
        a->video[a->nvideo].off = off;
        a->video[a->nvideo].len = len;
        a->nvideo++;
    } else if (s == ps->astream && id[2] == 'w' && id[3] == 'b' && len) {
        if (a->naudio == *acap) {
            *acap = *acap ? *acap * 2 : 256;
            void *n = realloc(a->audio_chunks, *acap * sizeof(*a->audio_chunks));
            void *m = n ? realloc(a->audio_frame, *acap * sizeof(*a->audio_frame)) : NULL;
            if (n)
                a->audio_chunks = n;
            if (!m)
                return -1;
            a->audio_frame = m;
        }
        a->audio_chunks[a->naudio].off = off;
        a->audio_chunks[a->naudio].len = len;
        a->audio_frame[a->naudio] = a->audio_frames;
        a->audio_frames += media_fmt_frames(&a->audio, len);
        a->naudio++;
    }
    return 0;
}

static int read_strl(struct media_avi *a, struct avi_parse *ps, uint32_t off, uint32_t end)
{
    uint8_t c[8], strh[48], strf[64];
    uint32_t type = 0, scale = 0, rate = 0;
    int have_strf = 0;
    uint32_t strf_len = 0;

    while (off + 8 <= end) {
        if (ps->rd(ps->ctx, off, c, 8) != 8)
            return -1;
        uint32_t len = le32(c + 4);
        if (!memcmp(c, "strh", 4) && len >= 28) {
            if (ps->rd(ps->ctx, off + 8, strh, 28) != 28)
                return -1;
            type = le32(strh);
            scale = le32(strh + 20);
            rate = le32(strh + 24);
        } else if (!memcmp(c, "strf", 4)) {
            strf_len = MIN(len, sizeof(strf));
            if (ps->rd(ps->ctx, off + 8, strf, strf_len) != (int)strf_len)
                return -1;
            have_strf = 1;
        }
        off += 8 + len + (len & 1);
    }
    int num = ps->nstreams++;
    if (type == le32((const uint8_t *)"vids") && ps->vstream < 0 && have_strf && strf_len >= 20) {
        ps->vstream = num;
        a->width = le32(strf + 4);
        a->height = le32(strf + 8);
        a->height = (uint32_t)((int32_t)a->height < 0 ? -(int32_t)a->height : (int32_t)a->height);
        a->mjpeg = !memcmp(strf + 16, "MJPG", 4) || !memcmp(strf + 16, "mjpg", 4);
        if (rate && scale && !a->usec_per_frame)
            a->usec_per_frame = (uint32_t)((uint64_t)scale * 1000000 / rate);
    } else if (type == le32((const uint8_t *)"auds") && ps->astream < 0 && have_strf) {
        if (media_audio_fmt_parse(strf, strf_len, &a->audio) == 0) {
            ps->astream = num;
            a->has_audio = true;
        }
    }
    return 0;
}

/* Walks the chunks of a LIST, descending into hdrl and strl. */
static int walk(struct media_avi *a, struct avi_parse *ps, uint32_t off, uint32_t end, int depth)
{
    uint8_t c[12];

    while (off + 8 <= end) {
        if (ps->rd(ps->ctx, off, c, 12) < 8)
            return -1;
        uint32_t len = le32(c + 4);
        uint32_t next = off + 8 + len + (len & 1);
        if (next > end || next < off)
            next = end;
        if (!memcmp(c, "LIST", 4)) {
            if (!memcmp(c + 8, "hdrl", 4) && depth == 0) {
                if (walk(a, ps, off + 12, next, 1))
                    return -1;
            } else if (!memcmp(c + 8, "strl", 4)) {
                if (read_strl(a, ps, off + 12, next))
                    return -1;
            } else if (!memcmp(c + 8, "movi", 4) && depth == 0) {
                ps->movi = off + 8;
                ps->movi_end = next;
            }
        } else if (!memcmp(c, "avih", 4) && len >= 4) {
            uint8_t h[4];
            if (ps->rd(ps->ctx, off + 8, h, 4) == 4 && le32(h))
                a->usec_per_frame = le32(h);
        } else if (!memcmp(c, "idx1", 4) && depth == 0 && ps->movi) {
            uint8_t *idx = malloc(len);
            uint32_t vcap = 0, acap = 0;
            if (!idx || ps->rd(ps->ctx, off + 8, idx, len) != (int)len) {
                free(idx);
                return -1;
            }
            /* Offsets are relative to the 'movi' fourcc, or absolute in
             * some writers; the first entry tells which. */
            uint32_t base = ps->movi;
            if (len >= 16 && le32(idx + 8) >= ps->movi)
                base = 0;
            for (uint32_t i = 0; i + 16 <= len; i += 16) {
                if (add_chunk(a, ps, idx + i, base + le32(idx + i + 8) + 8, le32(idx + i + 12), &vcap, &acap)) {
                    free(idx);
                    return -1;
                }
            }
            free(idx);
        }
        off = next;
    }
    return 0;
}

static int walk_movi(struct media_avi *a, struct avi_parse *ps, uint32_t off, uint32_t end,
                     uint32_t *vcap, uint32_t *acap)
{
    uint8_t c[12];

    while (off + 8 <= end) {
        if (ps->rd(ps->ctx, off, c, 12) < 8)
            return -1;
        uint32_t len = le32(c + 4);
        if (!memcmp(c, "LIST", 4)) {
            if (walk_movi(a, ps, off + 12, MIN(end, off + 8 + len), vcap, acap))
                return -1;
        } else if (add_chunk(a, ps, c, off + 8, len, vcap, acap)) {
            return -1;
        }
        off += 8 + len + (len & 1);
    }
    return 0;
}

int media_avi_open(media_read_fn rd, void *ctx, uint32_t file_size, struct media_avi *a)
{
    struct avi_parse ps = { rd, ctx, -1, -1, 0, 0, 0 };
    uint8_t h[12];

    memset(a, 0, sizeof(*a));
    if (rd(ctx, 0, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "AVI ", 4))
        return -1;
    uint32_t end = MIN(file_size, 8 + le32(h + 4));
    if (walk(a, &ps, 12, end, 0) || ps.vstream < 0 || !ps.movi)
        goto fail;
    if (!a->nvideo) {
        uint32_t vcap = 0, acap = 0;
        a->naudio = a->audio_frames = 0;
        if (walk_movi(a, &ps, ps.movi + 4, ps.movi_end, &vcap, &acap))
            goto fail;
    }
    if (!a->nvideo || !a->width || !a->height)
        goto fail;
    if (!a->usec_per_frame)
        a->usec_per_frame = 100000;
    return 0;
fail:
    media_avi_close(a);
    return -1;
}

void media_avi_close(struct media_avi *a)
{
    free(a->video);
    free(a->audio_chunks);
    free(a->audio_frame);
    memset(a, 0, sizeof(*a));
}
