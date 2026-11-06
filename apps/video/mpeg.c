/*
 * MPEG-1 program streams for Videos (see mpeg.h).
 *
 * pl_mpeg's demuxer reads the file through a buffer whose load callback
 * reads READ_STEP bytes at a time, so a refill never stalls for long, and
 * mpeg_prefetch() tops it up between frames, as the Motion-JPEG player
 * does with its window. Video packets go to pl_mpeg's video decoder; audio
 * packets collect in `afifo` for media_mp2_decode(). pl_mpeg's own audio
 * decoder is float and is never used.
 *
 * pl_mpeg keeps its times as doubles; on this soft-float CPU those are a
 * handful of operations per packet or frame (third_party/compiler-rt).
 */

#include "mpeg.h"

#include <glaze/fs.h>
#include <glaze/media.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define PLM_NO_STDIO
#define PLM_BUFFER_DEFAULT_SIZE (64 * 1024)
#define PL_MPEG_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
/* Hidden, so that --export-dynamic doesn't keep what this file doesn't
 * call (pl_mpeg's float audio decoder, its player, RGB conversions). */
#pragma GCC visibility push(hidden)
#include "pl_mpeg.h"
#pragma GCC visibility pop
#pragma GCC diagnostic pop

#define READ_STEP       4096        /* one file server read */
#define READ_AHEAD      (48 * 1024) /* prefetch up to this much unread */
#define AFIFO_SIZE      (16 * 1024) /* audio packets waiting to be decoded */
#define AUDIO_PROBE     256         /* packets to look through for the audio format */

static int fd = -1;
static uint32_t file_size, file_pos;    /* file_pos: just after what is buffered */
static uint32_t fs_pos;                 /* the file server's position */
static uint8_t *rbuf;

static plm_buffer_t *fbuf, *vbuf;
static plm_demux_t *demux;
static plm_video_t *video;
static plm_frame_t *last;
static double start_time;

static struct mpeg_info info;
static struct media_mp2 *mp2;
static struct media_mp2_info afmt;
static uint8_t *afifo;
static uint32_t aoff, alen;             /* unread bytes are [aoff, alen) */
static bool audio_eof;
static bool audio_timed;                /* audio_t0 set from a packet's PTS */
static uint32_t audio_t0, audio_frames; /* ms of the first frame, frames since */
static uint32_t audio_from;             /* after a seek, drop sound before this */

/* ---- file -------------------------------------------------------------------- */

static void file_load(plm_buffer_t *b, void *user)
{
    (void)user;
    if (file_pos >= file_size) {
        b->has_ended = TRUE;
        return;
    }
    if (fs_pos != file_pos && fs_seek(fd, (int32_t)file_pos, FS_SEEK_SET) < 0) {
        b->has_ended = TRUE;
        return;
    }
    fs_pos = file_pos;
    int n = fs_read(fd, rbuf, MIN(READ_STEP, file_size - file_pos));
    if (n <= 0) {
        b->has_ended = TRUE;
        return;
    }
    fs_pos += (uint32_t)n;
    file_pos += (uint32_t)n;
    plm_buffer_write(b, rbuf, (size_t)n);
}

static void file_seek(plm_buffer_t *b, size_t off, void *user)
{
    (void)b, (void)user;
    file_pos = (uint32_t)off;
}

static size_t file_tell(plm_buffer_t *b, void *user)
{
    (void)b, (void)user;
    return file_pos;
}

void mpeg_prefetch(void)
{
    if (fbuf && file_pos < file_size && plm_buffer_get_remaining(fbuf) < READ_AHEAD)
        file_load(fbuf, NULL);
}

/* ---- packets ------------------------------------------------------------------ */

static uint32_t to_ms(double t)
{
    return t > 0 ? (uint32_t)(t * 1000.0 + 0.5) : 0;
}

static void audio_packet(const plm_packet_t *p)
{
    if (!info.has_audio)
        return;
    if (!audio_timed) {
        /* The first packet after a seek starts the sound, with its PTS
         * (that of the first frame that begins in it). */
        if (p->pts == PLM_PACKET_INVALID_TS)
            return;
        uint32_t t = to_ms(p->pts - start_time);
        if (t < audio_from)
            return;
        audio_t0 = t;
        audio_frames = 0;
        audio_timed = true;
        aoff = alen = 0;
    }
    if (alen + p->length > AFIFO_SIZE) {
        memmove(afifo, afifo + aoff, alen - aoff);
        alen -= aoff;
        aoff = 0;
    }
    if (alen + p->length > AFIFO_SIZE)
        aoff = alen = 0;        /* nobody is listening: keep the newest */
    uint32_t n = MIN((uint32_t)p->length, AFIFO_SIZE);
    memcpy(afifo + alen, p->data, n);
    alen += n;
}

/* Demuxes until a packet of type `want`, handing out the others. */
static void read_packets(int want)
{
    plm_packet_t *p;

    while ((p = plm_demux_decode(demux))) {
        if (p->type == PLM_DEMUX_PACKET_VIDEO_1)
            plm_buffer_write(vbuf, p->data, p->length);
        else if (p->type == PLM_DEMUX_PACKET_AUDIO_1)
            audio_packet(p);
        if (p->type == want)
            return;
    }
    if (plm_demux_has_ended(demux)) {
        plm_buffer_signal_end(vbuf);
        audio_eof = true;
    }
}

static void video_load(plm_buffer_t *b, void *user)
{
    (void)b, (void)user;
    read_packets(PLM_DEMUX_PACKET_VIDEO_1);
}

/* ---- audio ---------------------------------------------------------------------- */

/* The frame at aoff, if it is whole and matches the stream's format;
 * otherwise skips ahead to a sync word. */
static bool audio_frame(struct media_mp2_info *h)
{
    while (alen - aoff >= 4) {
        if (!media_mp2_header(afifo + aoff, alen - aoff, h) && h->rate == afmt.rate &&
            h->channels == afmt.channels && h->lsf == afmt.lsf)
            return alen - aoff >= h->frame_len;
        aoff++;
    }
    return false;
}

int mpeg_audio(int16_t *pcm, uint32_t *ms)
{
    struct media_mp2_info h;

    if (!info.has_audio)
        return 0;
    while (!audio_frame(&h)) {
        if (audio_eof)
            return 0;
        uint32_t had = alen;
        read_packets(PLM_DEMUX_PACKET_AUDIO_1);
        if (alen == had && audio_eof)
            return 0;
    }
    int n = media_mp2_decode(mp2, afifo + aoff, h.frame_len, pcm);
    aoff += h.frame_len;
    if (n <= 0)
        return 0;
    *ms = audio_t0 + (uint32_t)((uint64_t)audio_frames * 1000 / afmt.rate);
    audio_frames += (uint32_t)n;
    return n;
}

/* Restarts the sound at the first packet from `ms` on. */
static void audio_reset(uint32_t ms)
{
    aoff = alen = 0;
    audio_eof = false;
    audio_timed = false;
    audio_from = ms;
    if (mp2)
        media_mp2_init(mp2);
}

/* The format of the first audio frame, demuxing (and keeping the video)
 * as far as it takes. */
static bool probe_audio(void)
{
    struct media_mp2_info h;

    for (int i = 0; i < AUDIO_PROBE && !audio_eof; i++) {
        while (alen - aoff >= 4) {
            if (!media_mp2_header(afifo + aoff, alen - aoff, &h)) {
                afmt = h;
                return true;
            }
            aoff++;
        }
        read_packets(PLM_DEMUX_PACKET_AUDIO_1);
    }
    return false;
}

/* ---- video ---------------------------------------------------------------------- */

bool mpeg_next_frame(uint32_t *ms)
{
    plm_frame_t *f = plm_video_decode(video);

    /* At the end, the last reference frame is still to be shown. pl_mpeg
     * returns it only when the last picture decoded was I or P, not when
     * the stream ends with B frames, which display before it. */
    if (!f && plm_buffer_has_ended(vbuf) && video->has_reference_frame && !video->assume_no_b_frames) {
        video->has_reference_frame = FALSE;
        f = &video->frame_backward;
        f->time = video->time;
        video->frames_decoded++;
        video->time = (double)video->frames_decoded / video->framerate;
    }
    if (!f)
        return false;
    last = f;
    *ms = to_ms(f->time);
    return true;
}

/* The type of the picture whose start code is next in vbuf, once it is
 * all there, or -1. */
static int next_picture_type(void)
{
    plm_video_t *v = video;

    if (v->start_code != PLM_START_PICTURE) {
        v->start_code = plm_buffer_find_start_code(vbuf, PLM_START_PICTURE);
        if (v->start_code == -1)
            return -1;
    }
    /* As plm_video_decode requires, up to the next picture (or the end);
     * then 10 bits of temporal reference and 3 of picture_coding_type. */
    if ((plm_buffer_has_start_code(vbuf, PLM_START_PICTURE) == -1 && !plm_buffer_has_ended(vbuf)) ||
        !plm_buffer_has(vbuf, 16))
        return -1;
    const uint8_t *hdr = vbuf->bytes + (vbuf->bit_index >> 3);
    return hdr[1] >> 3 & 7;
}

/* Passes over the picture next in vbuf. */
static void skip_picture(void)
{
    video->start_code = plm_buffer_find_start_code(vbuf, PLM_START_PICTURE);
}

bool mpeg_skip_b(uint32_t *next_ms)
{
    if (next_picture_type() != PLM_VIDEO_PICTURE_TYPE_B)
        return false;
    skip_picture();
    video->frames_decoded++;
    video->time = (double)video->frames_decoded / video->framerate;
    *next_ms += info.frame_us / 1000;
    return true;
}

/* BT.601 with pl_mpeg's coefficients (plm_frame_to_rgb), all from tables:
 * luma and the chroma terms by sample value, and per component the
 * clamped sum's bits of the RGB565 pixel, indexed from -CLAMP_LO. */
#define CLAMP_LO        288
#define CLAMP_N         (CLAMP_LO + 256 + 288)
struct yuv_tables {
    uint16_t r[CLAMP_N], g[CLAMP_N], b[CLAMP_N];
    int16_t y[256], rv[256], gu[256], gv[256], bu[256];
};
static struct yuv_tables *yt;

static bool make_tables(void)
{
    if (yt)
        return true;
    yt = malloc(sizeof(*yt));
    if (!yt)
        return false;
    for (int i = 0; i < CLAMP_N; i++) {
        int v = i - CLAMP_LO;
        v = v < 0 ? 0 : v > 255 ? 255 : v;
        yt->r[i] = (uint16_t)((v >> 3) << 11);
        yt->g[i] = (uint16_t)((v >> 2) << 5);
        yt->b[i] = (uint16_t)(v >> 3);
    }
    for (int i = 0; i < 256; i++) {
        int c = i - 128;
        yt->y[i] = (int16_t)(((i - 16) * 76309) >> 16);
        yt->rv[i] = (int16_t)((c * 104597) >> 16);
        yt->gu[i] = (int16_t)((c * 25674) >> 16);
        yt->gv[i] = (int16_t)((c * 53278) >> 16);
        yt->bu[i] = (int16_t)((c * 132201) >> 16);
    }
    return true;
}

/* One pixel of luma `y` with the chroma terms already applied to the
 * component tables. */
#define YUV_PX(R, G, B, yy) (l = ytab[yy], (uint16_t)((R)[l] | (G)[l] | (B)[l]))

void mpeg_convert(uint16_t *px)
{
    const plm_frame_t *f = last;

    if (!f)
        return;
    int w = (int)info.width, h = (int)info.height;
    int yw = (int)f->y.width, cw = (int)f->cb.width;
    const int16_t *ytab = yt->y;
    const uint16_t *rt = yt->r + CLAMP_LO, *gt = yt->g + CLAMP_LO, *bt = yt->b + CLAMP_LO;

    /* Pairs of rows and columns; an odd last column or row is drawn from
     * the first pixel of its pair. */
    for (int row = 0; row < h; row += 2) {
        const uint8_t *y0 = f->y.data + row * yw, *y1 = y0 + yw;
        const uint8_t *cb = f->cb.data + (row >> 1) * cw, *cr = f->cr.data + (row >> 1) * cw;
        uint16_t *d0 = px + row * w, *d1 = row + 1 < h ? d0 + w : NULL;
        int l, col;
        for (col = 0; col + 1 < w; col += 2) {
            int u = *cb++, v = *cr++;
            const uint16_t *R = rt + yt->rv[v], *G = gt - yt->gu[u] - yt->gv[v], *B = bt + yt->bu[u];
            d0[0] = YUV_PX(R, G, B, y0[0]);
            d0[1] = YUV_PX(R, G, B, y0[1]);
            if (d1) {
                d1[0] = YUV_PX(R, G, B, y1[0]);
                d1[1] = YUV_PX(R, G, B, y1[1]);
                d1 += 2;
            }
            d0 += 2;
            y0 += 2;
            y1 += 2;
        }
        if (col < w) {
            int u = *cb, v = *cr;
            const uint16_t *R = rt + yt->rv[v], *G = gt - yt->gu[u] - yt->gv[v], *B = bt + yt->bu[u];
            *d0 = YUV_PX(R, G, B, y0[0]);
            if (d1)
                *d1 = YUV_PX(R, G, B, y1[0]);
        }
    }
}

bool mpeg_seek(uint32_t ms, uint32_t *shown_ms)
{
    plm_video_t *v = video;
    plm_packet_t *p = plm_demux_seek(demux, ms / 1000.0, PLM_DEMUX_PACKET_VIDEO_1, TRUE);

    if (!p)
        return false;
    /* The packet begins the intra picture, and its PTS is that picture's. */
    plm_video_rewind(v);
    double t0 = p->pts - start_time;
    v->frames_decoded = (int)(t0 * v->framerate + 0.5);
    v->time = (double)v->frames_decoded / v->framerate;
    plm_buffer_write(vbuf, p->data, p->length);
    audio_reset(ms);
    last = NULL;

    /* Decode the intra picture, which is the first reference and shows
     * nothing yet (as in plm_video_decode). */
    if (next_picture_type() != PLM_VIDEO_PICTURE_TYPE_INTRA)
        return false;
    plm_buffer_discard_read_bytes(vbuf);
    plm_video_decode_picture(v);
    v->has_reference_frame = TRUE;
    /* In an open GOP, the B pictures after it display before it and
     * predict from the GOP before, which isn't there: pass over them
     * (pl_mpeg's plm_seek shows them, mislabelled with the intra frame's
     * time). */
    while (next_picture_type() == PLM_VIDEO_PICTURE_TYPE_B)
        skip_picture();

    /* Then decode up to the frame on screen at `ms`. */
    uint32_t t, frame_ms = info.frame_us / 1000;
    if (!mpeg_next_frame(&t))
        return false;
    while (t + frame_ms <= ms) {
        plm_frame_t *keep = last;
        if (!mpeg_next_frame(&t)) {
            last = keep;
            break;
        }
    }
    *shown_ms = t;
    return true;
}

/* ---- open and close ---------------------------------------------------------------- */

void mpeg_close(void)
{
    if (video)
        plm_video_destroy(video);       /* and vbuf */
    else if (vbuf)
        plm_buffer_destroy(vbuf);
    if (demux)
        plm_demux_destroy(demux);       /* and fbuf */
    else if (fbuf)
        plm_buffer_destroy(fbuf);
    video = NULL;
    demux = NULL;
    vbuf = fbuf = NULL;
    last = NULL;
    free(mp2);
    free(yt);
    yt = NULL;
    free(afifo);
    free(rbuf);
    mp2 = NULL;
    afifo = rbuf = NULL;
    fd = -1;
}

bool mpeg_open(int file, uint32_t size, struct mpeg_info *mi)
{
    mpeg_close();
    fd = file;
    file_size = size;
    file_pos = 0;
    fs_pos = UINT32_MAX;
    memset(&info, 0, sizeof(info));
    audio_reset(0);
    rbuf = malloc(READ_STEP);
    afifo = malloc(AFIFO_SIZE);
    if (!rbuf || !afifo || !make_tables())
        goto fail;
    fbuf = plm_buffer_create_with_callbacks(file_load, file_seek, file_tell, size, NULL);
    if (!fbuf)
        goto fail;
    demux = plm_demux_create(fbuf, TRUE);
    if (!demux || !plm_demux_has_headers(demux) || plm_demux_get_num_video_streams(demux) < 1)
        goto fail;
    start_time = plm_demux_get_start_time(demux, PLM_DEMUX_PACKET_VIDEO_1);
    double dur = plm_demux_get_duration(demux, PLM_DEMUX_PACKET_VIDEO_1);

    vbuf = plm_buffer_create_with_capacity(PLM_BUFFER_DEFAULT_SIZE);
    if (!vbuf)
        goto fail;
    plm_buffer_set_load_callback(vbuf, video_load, NULL);
    info.has_audio = plm_demux_get_num_audio_streams(demux) > 0;
    video = plm_video_create_with_buffer(vbuf, TRUE);
    if (!video || !plm_video_has_header(video) || plm_video_get_framerate(video) <= 0)
        goto fail;
    info.width = (uint32_t)plm_video_get_width(video);
    info.height = (uint32_t)plm_video_get_height(video);
    info.frame_us = (uint32_t)(1000000.0 / plm_video_get_framerate(video));
    info.duration_ms = to_ms(dur) + info.frame_us / 1000;

    if (info.has_audio) {
        mp2 = malloc(sizeof(*mp2));
        info.has_audio = mp2 && probe_audio();
        if (info.has_audio) {
            media_mp2_init(mp2);
            info.rate = afmt.rate;
            info.channels = afmt.channels;
        }
    }
    if (!info.width || !info.height)
        goto fail;
    *mi = info;
    return true;
fail:
    mpeg_close();
    return false;
}
