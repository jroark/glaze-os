/*
 * Videos: the Motion-JPEG AVI files in /Movies, played full width with
 * their sound.
 *
 * Audio (PCM or IMA ADPCM) goes to the audio server as a PCM stream
 * (glaze/audio.h), and the stream's clock, which counts what has been
 * heard, decides which frame is due; frames that are late are skipped, so
 * the picture keeps pace with the sound however slowly JPEG decodes.
 * Without an audio track, or once the sound runs out, the wall clock
 * takes over. Seeking restarts the stream at the chosen time.
 *
 * Reading costs more than decoding: the file server manages about 70 KB/s,
 * and less when every chunk is a seek and a short read. So the movie is
 * read front to back into a window that video frames and sound chunks are
 * copied out of, topped up while waiting for the next frame.
 *
 * The list shows each video's first frame, decoded in the background.
 */

#include <glaze/audio.h>
#include <glaze/fs.h>
#include <glaze/media.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_FAILURE_STRINGS
#define STBI_ASSERT(x)          ((void)0)
#define STBI_MAX_DIMENSIONS     1024
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "stb_image.h"
#pragma GCC diagnostic pop
/* With STBI_NO_THREAD_LOCALS, stb declares three _thread functions it never
 * defines; that warning comes at the end of the file. */
#pragma GCC diagnostic ignored "-Wunused-function"

#define MOVIES          "/Movies"
#define MAX_VIDEOS      32
#define ROW_H           58
#define THUMB_W         64
#define THUMB_H         48
#define MAX_W           640
#define MAX_H           480
#define AUDIO_AHEAD_MS  800         /* sound kept queued in the server */
#define OVERLAY_MS      3000        /* controls hide after this while playing */
#define SEEK_STEP_MS    10000
#define WIN_SIZE        (96 * 1024) /* read-ahead window */
#define WIN_STEP        4096        /* one file server read */
#define WIN_AHEAD       (48 * 1024) /* prefetch up to this far past what is needed */
#define WIN_JUMP        (64 * 1024) /* further ahead than this, seek instead */

/* Overlay layout. */
#define BAR_Y           (UI_NAV_H + 14)
#define BAR_X0          44
#define BAR_X1          (UI_W - 44)
#define PANEL           gfx_r(70, UI_H - 54, UI_W - 140, 44)

struct video {
    char name[FS_NAME_MAX];
    char title[FS_NAME_MAX];
    uint32_t duration_ms;
    uint16_t thumb_px[THUMB_W * THUMB_H];
    bool has_thumb, tried;
};

static struct video videos[MAX_VIDEOS];
static int nvideos;

enum view { V_LIST, V_PLAYER };
static enum view view = V_LIST;
static struct ui_list list;

/* The open video. */
static int fd = -1;
static struct media_avi avi;
static int current;
static uint8_t *jbuf, *jfixed;      /* a frame as read, and with Huffman tables */
static uint32_t jcap;
static uint16_t *frame_px;
static gfx_surface frame;
static int shown = -1;              /* frame index on screen */
static bool frame_ok;
static uint8_t *win;
static uint32_t win_off, win_len;   /* file range held in win */

/* Playback clock. */
static bool playing, ended;
static uint32_t base_ms;            /* media time when the clock started */
static uint32_t paused_ms;
static bool audio_on;               /* clock from the audio stream */
static uint32_t wall_t0;

/* Audio feed. */
static uint32_t next_chunk;         /* next audio chunk to send */
static uint32_t skip_frames;        /* to drop from the first chunk after a seek */
static uint8_t *abuf;
static uint32_t abuf_cap;
static int16_t *pcm;
static uint32_t pcm_off, pcm_len;   /* bytes waiting for the server */
static bool audio_eof;

/* Controls. */
static bool overlay = true;
static uint32_t overlay_until;
static gfx_rect done_rect;
static bool pressed_done, pressed_play, scrubbing;
static uint32_t scrub_ms;

/* ---- files ---------------------------------------------------------------- */

static int fs_reader(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    (void)ctx;
    if (fs_seek(fd, (int32_t)off, FS_SEEK_SET) < 0)
        return GZ_EIO;
    return fs_read(fd, buf, len);
}

static int video_cmp(const void *a, const void *b)
{
    return strcasecmp(((const struct video *)a)->name, ((const struct video *)b)->name);
}

static void list_videos(void)
{
    struct fs_dirent ents[8];
    uint32_t skip = 0;
    int n;

    while (nvideos < MAX_VIDEOS && (n = fs_list(MOVIES, skip, ents, 8)) > 0) {
        for (int i = 0; i < n && nvideos < MAX_VIDEOS; i++) {
            const char *dot = strrchr(ents[i].name, '.');
            if (ents[i].type != FS_FILE || !dot || strcasecmp(dot, ".avi"))
                continue;
            struct video *v = &videos[nvideos++];
            memcpy(v->name, ents[i].name, sizeof(v->name));
            memcpy(v->title, ents[i].name, sizeof(v->title));
            v->title[dot - ents[i].name] = 0;
        }
        skip += (uint32_t)n;
    }
    qsort(videos, (size_t)nvideos, sizeof(videos[0]), video_cmp);
}

static void close_video(void)
{
    media_avi_close(&avi);
    free(win);
    win = NULL;
    win_len = 0;
    if (fd >= 0)
        fs_close(fd);
    fd = -1;
    free(jbuf);
    free(jfixed);
    free(frame_px);
    free(abuf);
    free(pcm);
    jbuf = jfixed = abuf = NULL;
    frame_px = NULL;
    pcm = NULL;
}

/* Opens video i and sizes the buffers for its largest chunks. */
static bool open_video(int i)
{
    char path[FS_PATH_MAX];
    struct fs_stat st;

    close_video();
    snprintf(path, sizeof(path), MOVIES "/%s", videos[i].name);
    if (fs_stat(path, &st) < 0 || (fd = fs_open(path, FS_O_READ)) < 0)
        return false;
    if (media_avi_open(fs_reader, NULL, st.size, &avi) < 0 || !avi.mjpeg ||
        avi.width > MAX_W || avi.height > MAX_H) {
        close_video();
        return false;
    }
    jcap = 0;
    for (uint32_t k = 0; k < avi.nvideo; k++)
        jcap = MAX(jcap, avi.video[k].len);
    abuf_cap = 0;
    for (uint32_t k = 0; k < avi.naudio; k++)
        abuf_cap = MAX(abuf_cap, avi.audio_chunks[k].len);
    win = malloc(WIN_SIZE);
    win_off = win_len = 0;
    jbuf = malloc(jcap);
    jfixed = malloc(jcap + MEDIA_JPEG_DHT_SIZE);
    frame_px = malloc(avi.width * avi.height * 2);
    if (avi.has_audio) {
        abuf = malloc(abuf_cap);
        pcm = malloc((media_fmt_frames(&avi.audio, abuf_cap) + avi.audio.block_frames) * avi.audio.channels * 2);
    }
    if (!win || !jbuf || !jfixed || !frame_px || (avi.has_audio && (!abuf || !pcm))) {
        close_video();
        return false;
    }
    gfx_surface_init(&frame, frame_px, (int)avi.width, (int)avi.height, (int)avi.width);
    videos[i].duration_ms = (uint32_t)((uint64_t)avi.nvideo * avi.usec_per_frame / 1000);
    return true;
}

/* The earliest file offset still needed: the next frame, the next sound
 * chunk, or `off`. */
static uint32_t keep_from(uint32_t off);

/* Reads one more WIN_STEP into the window, first dropping what is before
 * `keep` (or, if that isn't enough, the oldest data) to make room. */
static bool win_fill(uint32_t keep)
{
    if (win_len + WIN_STEP > WIN_SIZE) {
        uint32_t drop = keep > win_off ? MIN(keep - win_off, win_len) : 0;
        drop = MAX(drop, win_len + WIN_STEP - WIN_SIZE);
        memmove(win, win + drop, win_len - drop);
        win_off += drop;
        win_len -= drop;
    }
    int n = fs_reader(NULL, win_off + win_len, win + win_len, WIN_STEP);
    if (n <= 0)
        return false;
    win_len += (uint32_t)n;
    return true;
}

/* Copies [off, off + len) of the file out of the window, reading forward. */
static bool win_read(uint32_t off, void *buf, uint32_t len)
{
    if (len > WIN_SIZE - WIN_STEP)
        return fs_reader(NULL, off, buf, len) == (int)len;
    if (off < win_off || off > win_off + win_len + WIN_JUMP) {
        win_off = off;
        win_len = 0;
    }
    while (off + len > win_off + win_len) {
        if (!win_fill(keep_from(off)))
            return false;
        if (off < win_off)      /* had to drop it to make room */
            return fs_reader(NULL, off, buf, len) == (int)len;
    }
    memcpy(buf, win + (off - win_off), len);
    return true;
}

/* Decodes video frame k into frame_px. */
static bool decode_frame(uint32_t k)
{
    const struct media_avi_chunk *c = &avi.video[k];
    int w, h, comp;

    if (!c->len || !win_read(c->off, jbuf, c->len))
        return false;
    const uint8_t *j = jbuf;
    int len = (int)c->len, fixed = media_jpeg_add_dht(jbuf, c->len, jfixed);
    if (fixed > 0) {
        j = jfixed;
        len = fixed;
    }
    uint8_t *rgb = stbi_load_from_memory(j, len, &w, &h, &comp, 3);
    if (!rgb)
        return false;
    if (w != (int)avi.width || h != (int)avi.height) {
        stbi_image_free(rgb);
        return false;
    }
    const uint8_t *p = rgb;
    for (int i = 0; i < w * h; i++, p += 3)
        frame_px[i] = (uint16_t)((p[0] & 0xF8) << 8 | (p[1] & 0xFC) << 3 | p[2] >> 3);
    stbi_image_free(rgb);
    return true;
}

/* First frame of video i, scaled into its thumbnail. */
static void load_thumb(int i)
{
    struct video *v = &videos[i];
    gfx_surface t;

    v->tried = true;
    if (!open_video(i))
        return;
    if (decode_frame(0)) {
        gfx_surface_init(&t, v->thumb_px, THUMB_W, THUMB_H, THUMB_W);
        gfx_fill(&t, gfx_r(0, 0, THUMB_W, THUMB_H), 0x000000);
        int th = THUMB_W * (int)avi.height / (int)avi.width;
        if (th > THUMB_H)
            th = THUMB_H;
        gfx_blit_scaled(&t, gfx_r(0, (THUMB_H - th) / 2, THUMB_W, th), &frame,
                        gfx_r(0, 0, (int)avi.width, (int)avi.height), 255);
        v->has_thumb = true;
    }
    close_video();
}

static int next_thumb(void)
{
    for (int i = 0; i < nvideos; i++)
        if (!videos[i].tried)
            return i;
    return -1;
}

/* ---- clock and audio ---------------------------------------------------------- */

static uint32_t duration(void)
{
    return (uint32_t)((uint64_t)avi.nvideo * avi.usec_per_frame / 1000);
}

static uint32_t clock_ms(void)
{
    uint32_t played;

    if (!playing)
        return paused_ms;
    if (audio_on && audio_stream_clock(&played, NULL) >= 0)
        return base_ms + played;
    return base_ms + (gz_time_ms() - wall_t0);
}

/* Continue on the wall clock from wherever the sound left off. */
static void to_wall_clock(void)
{
    uint32_t now = clock_ms();

    audio_on = false;
    base_ms = now;
    wall_t0 = gz_time_ms();
}

static void stop_clock(void)
{
    paused_ms = clock_ms();
    playing = false;
    if (avi.has_audio)
        audio_stream_close();
    audio_on = false;
}

/* Starts playing at `ms`: video from the frame there, sound from the
 * chunk holding that moment. */
static void start_at(uint32_t ms)
{
    uint32_t f = (uint32_t)((uint64_t)ms * 1000 / avi.usec_per_frame);

    if (f >= avi.nvideo)
        f = 0, ms = 0;
    base_ms = ms;
    wall_t0 = gz_time_ms();
    playing = true;
    ended = false;
    shown = -1;
    pcm_off = pcm_len = 0;
    audio_on = false;
    audio_eof = true;
    if (avi.has_audio && avi.naudio && audio_stream_open(avi.audio.rate, avi.audio.channels) == 0) {
        uint32_t af = (uint32_t)((uint64_t)ms * avi.audio.rate / 1000);
        uint32_t k = 0;
        while (k + 1 < avi.naudio && avi.audio_frame[k + 1] <= af)
            k++;
        next_chunk = k;
        skip_frames = af > avi.audio_frame[k] ? af - avi.audio_frame[k] : 0;
        audio_on = true;
        audio_eof = false;
    }
}

/* Decodes audio chunk k into pcm, dropping skip_frames from its start. */
static void decode_chunk(uint32_t k)
{
    const struct media_avi_chunk *c = &avi.audio_chunks[k];
    const struct media_audio_fmt *f = &avi.audio;
    uint32_t frames = 0;

    pcm_off = pcm_len = 0;
    if (!win_read(c->off, abuf, c->len))
        return;
    for (uint32_t at = 0; at < c->len; ) {
        uint32_t unit = f->format == MEDIA_FMT_IMA ? MIN(f->block_align, c->len - at)
                                                   : (c->len - at) / f->block_align * f->block_align;
        if (!unit)
            break;
        frames += (uint32_t)media_decode(f, abuf + at, unit, pcm + frames * f->channels);
        at += unit;
    }
    uint32_t drop = MIN(skip_frames, frames);
    skip_frames = 0;
    pcm_off = drop * f->channels * 2;
    pcm_len = (frames - drop) * f->channels * 2;
}

static uint32_t keep_from(uint32_t off)
{
    if (shown + 1 < (int)avi.nvideo)
        off = MIN(off, avi.video[shown + 1].off);
    if (audio_on && !audio_eof && next_chunk < avi.naudio)
        off = MIN(off, avi.audio_chunks[next_chunk].off);
    return off;
}

/* Tops up the window while there is time before the next frame. */
static void prefetch(void)
{
    uint32_t keep = keep_from(win_off + win_len);

    if (keep >= win_off && win_off + win_len < keep + WIN_AHEAD)
        win_fill(keep);
}

/* Keeps AUDIO_AHEAD_MS of sound queued in the server. */
static void feed_audio(void)
{
    uint32_t played, queued;

    while (audio_on && !audio_eof) {
        if (pcm_len) {
            int n = audio_stream_write((uint8_t *)pcm + pcm_off, pcm_len);
            if (n < 0) {
                to_wall_clock();        /* the Music player took the output */
                return;
            }
            if (n == 0)
                return;
            pcm_off += (uint32_t)n;
            pcm_len -= (uint32_t)n;
            continue;
        }
        if (audio_stream_clock(&played, &queued) < 0 || queued > AUDIO_AHEAD_MS)
            return;
        if (next_chunk >= avi.naudio) {
            audio_eof = true;
            audio_stream_write(NULL, 0);   /* short tail: start now */
            return;
        }
        decode_chunk(next_chunk++);
    }
    /* All sound sent: once it has been heard, the wall clock carries on. */
    if (audio_on && audio_eof && audio_stream_clock(&played, &queued) >= 0 && queued == 0)
        to_wall_clock();
}

/* ---- drawing ---------------------------------------------------------------- */

static void fmt_time(char *buf, size_t n, uint32_t ms, bool minus)
{
    uint32_t sec = ms / 1000;

    snprintf(buf, n, "%s%u:%02u", minus ? "-" : "", sec / 60, sec % 60);
}

static void triangle(gfx_surface *s, int x, int y, int w, int h, uint32_t rgb)
{
    for (int i = 0; i < w; i++) {
        int inset = i * h / (2 * w);
        gfx_fill(s, gfx_r(x + i, y + inset, 1, h - 2 * inset), rgb);
    }
}

static void draw_video(gfx_surface *s, int i, gfx_rect r, bool sel, void *ctx)
{
    struct video *v = &videos[i];
    const gfx_font *f = ui_font(FONT_B14), *sf = ui_font(FONT_R12);
    char dur[16];

    (void)ctx;
    ui_draw_list_row(s, r, sel);
    gfx_rect tr = gfx_r(6, r.y + (ROW_H - THUMB_H) / 2, THUMB_W, THUMB_H);
    if (v->has_thumb) {
        gfx_surface t;
        gfx_surface_init(&t, v->thumb_px, THUMB_W, THUMB_H, THUMB_W);
        gfx_blit(s, tr.x, tr.y, &t, gfx_r(0, 0, THUMB_W, THUMB_H));
    } else {
        gfx_fill(s, tr, 0x202020);
    }
    int tx = tr.x + THUMB_W + 10;
    gfx_text_fit(s, tx, r.y + 11, UI_W - tx - 24, f, v->title, sel ? 0xFFFFFF : UI_TEXT, 255);
    if (v->duration_ms) {
        fmt_time(dur, sizeof(dur), v->duration_ms, false);
        gfx_text(s, tx, r.y + 31, sf, dur, sel ? 0xFFFFFF : 0x808080, 255);
    }
    ui_draw_chevron(s, UI_W - 16, r.y + ROW_H / 2 - 5, sel ? 0xFFFFFF : 0x9A9A9A);
}

static void draw_list(gfx_surface *s)
{
    gfx_rect body = list.area;
    const gfx_font *sf = ui_font(FONT_R12);

    gfx_set_clip(s, body);
    gfx_fill(s, body, 0xFFFFFF);
    ui_list_draw(s, &list, draw_video, NULL);
    if (!nvideos) {
        gfx_text_center(s, 0, UI_NAV_H + 100, UI_W, ui_font(FONT_B17), "No Videos", 0x666666, 255);
        gfx_text_center(s, 0, UI_NAV_H + 126, UI_W, sf, "Motion-JPEG AVI files you add", 0x888888, 255);
        gfx_text_center(s, 0, UI_NAV_H + 144, UI_W, sf, "to " MOVIES " appear here.", 0x888888, 255);
    }
    gfx_reset_clip(s);
    ui_draw_navbar(s, "Videos", NULL, NULL, 1);
}

static gfx_rect video_rect(void)
{
    int w = UI_W, h = (int)((uint32_t)UI_W * avi.height / avi.width);

    if (h > UI_H) {
        h = UI_H;
        w = (int)((uint32_t)UI_H * avi.width / avi.height);
    }
    return gfx_r((UI_W - w) / 2, (UI_H - h) / 2, w, h);
}

static uint32_t shown_ms(void)
{
    return scrubbing ? scrub_ms : MIN(clock_ms(), duration());
}

static void draw_overlay(gfx_surface *s)
{
    static uint16_t barbuf[UI_W * (UI_NAV_H + 28)];
    const gfx_font *nf = ui_font(FONT_B11);
    gfx_surface bar;
    char a[16], b[16];
    int bh = UI_NAV_H + 28;

    /* Translucent top bar: Done, then the scrubber under it. */
    gfx_surface_init(&bar, barbuf, UI_W, bh, UI_W);
    gfx_vgradient(&bar, gfx_r(0, 0, UI_W, bh), 0x3A3A3A, 0x0A0A0A, 255);
    done_rect = ui_draw_bar_button(&bar, "Done", false, pressed_done, true, 1);
    gfx_text_center(&bar, 0, (UI_NAV_H - ui_font(FONT_B14)->line_height) / 2, UI_W, ui_font(FONT_B14),
                    videos[current].title, 0xFFFFFF, 255);
    uint32_t pos = shown_ms(), dur = duration();
    fmt_time(a, sizeof(a), pos, false);
    fmt_time(b, sizeof(b), dur - MIN(pos, dur), true);
    gfx_text(&bar, 6, BAR_Y - nf->line_height / 2, nf, a, 0xFFFFFF, 255);
    gfx_text(&bar, UI_W - 6 - gfx_text_width(nf, b), BAR_Y - nf->line_height / 2, nf, b, 0xFFFFFF, 255);
    int kx = BAR_X0 + (dur ? (int)((uint64_t)MIN(pos, dur) * (BAR_X1 - BAR_X0) / dur) : 0);
    gfx_round_rect(&bar, gfx_r(BAR_X0, BAR_Y - 2, BAR_X1 - BAR_X0, 5), 2, 0x4A4A4A, 255);
    gfx_round_rect(&bar, gfx_r(BAR_X0, BAR_Y - 2, kx - BAR_X0 + 2, 5), 2, 0xD8D8D8, 255);
    gfx_circle(&bar, kx, BAR_Y, scrubbing ? 8 : 6, 0xFFFFFF, 255);
    gfx_blit_alpha(s, 0, 0, &bar, gfx_r(0, 0, UI_W, bh), 190);

    /* Play/pause on a rounded glass panel. */
    gfx_rect p = PANEL;
    gfx_round_rect(s, p, 8, 0x000000, 150);
    gfx_round_rect(s, gfx_r(p.x + 1, p.y + 1, p.w - 2, p.h / 2), 7, 0xFFFFFF, 25);
    int cx = p.x + p.w / 2, cy = p.y + p.h / 2;
    if (pressed_play)
        gfx_circle(s, cx, cy, 18, 0xFFFFFF, 60);
    if (playing) {
        gfx_fill(s, gfx_r(cx - 7, cy - 9, 5, 18), 0xFFFFFF);
        gfx_fill(s, gfx_r(cx + 2, cy - 9, 5, 18), 0xFFFFFF);
    } else {
        triangle(s, cx - 7, cy - 10, 17, 20, 0xFFFFFF);
    }
}

static void draw_player(gfx_surface *s)
{
    gfx_rect vr = video_rect();

    gfx_fill(s, gfx_r(0, 0, UI_W, UI_H), 0x000000);
    if (frame_ok)
        gfx_blit_scaled(s, vr, &frame, gfx_r(0, 0, frame.w, frame.h), 255);
    if (overlay)
        draw_overlay(s);
}

static void draw_current(gfx_surface *s)
{
    if (view == V_LIST)
        draw_list(s);
    else
        draw_player(s);
}

static void redraw(void)
{
    draw_current(&ui.surf);
    ui_invalidate_all();
    ui_present();
}

/* Only the picture changed: redraw and present just that, when the
 * controls aren't over it. */
static void present_frame(void)
{
    if (overlay) {
        redraw();
        return;
    }
    gfx_rect vr = video_rect();
    gfx_blit_scaled(&ui.surf, vr, &frame, gfx_r(0, 0, frame.w, frame.h), 255);
    ui_invalidate(vr);
    ui_present();
}

/* ---- playback ------------------------------------------------------------------ */

static void show_overlay(void)
{
    overlay = true;
    overlay_until = gz_time_ms() + OVERLAY_MS;
}

static void play_video(int i)
{
    current = i;
    if (!open_video(i))
        return;
    frame_ok = decode_frame(0);
    shown = 0;
    view = V_PLAYER;
    show_overlay();
    ui_transition(draw_current, true);
    start_at(0);
    redraw();
}

static void leave_player(void)
{
    stop_clock();
    close_video();
    frame_ok = false;
    view = V_LIST;
    ui_transition(draw_current, false);
}

static void toggle_play(void)
{
    if (playing) {
        stop_clock();
        show_overlay();
    } else {
        start_at(ended ? 0 : paused_ms);
        show_overlay();
    }
}

static void seek_to(uint32_t ms)
{
    bool was = playing || ended;

    stop_clock();
    ms = MIN(ms, duration());
    uint32_t f = (uint32_t)((uint64_t)ms * 1000 / avi.usec_per_frame);
    if (f < avi.nvideo && decode_frame(f)) {
        frame_ok = true;
        shown = (int)f;
    }
    paused_ms = ms;
    ended = false;
    if (was)
        start_at(ms);
}

/* Shows the frame that is due; at the end, stops with the controls up. */
static void play_step(void)
{
    feed_audio();
    uint32_t t = clock_ms();
    uint32_t f = (uint32_t)((uint64_t)t * 1000 / avi.usec_per_frame);

    if (f >= avi.nvideo) {
        if (!audio_on || audio_eof) {
            stop_clock();
            paused_ms = duration();
            ended = true;
            show_overlay();
            redraw();
        }
        return;
    }
    if ((int)f != shown) {
        if (decode_frame(f))
            frame_ok = true;
        shown = (int)f;
        present_frame();
    } else if ((uint64_t)(f + 1) * avi.usec_per_frame / 1000 > t + 60) {
        prefetch();
    } else if (overlay && t / 1000 != (t + 40) / 1000) {
        redraw();       /* the elapsed time ticks over */
    }
    if (overlay && playing && gz_time_ms() > overlay_until && !scrubbing) {
        overlay = false;
        redraw();
    }
}

/* ---- events -------------------------------------------------------------------- */

static void list_event(const struct gz_input_event *e)
{
    bool dirty = false;
    int i = ui_list_event(&list, e, &dirty);

    if (i >= 0)
        play_video(i);
    else if (dirty)
        redraw();
}

static uint32_t scrub_at(int x)
{
    x = MAX(BAR_X0, MIN(BAR_X1, x));
    return (uint32_t)((uint64_t)(x - BAR_X0) * duration() / (BAR_X1 - BAR_X0));
}

static void player_event(const struct gz_input_event *e)
{
    gfx_rect p = PANEL;

    if (e->type == GZ_EV_TOUCH_DOWN) {
        if (!overlay) {
            show_overlay();
            redraw();
            return;
        }
        overlay_until = gz_time_ms() + OVERLAY_MS;
        if (gfx_rect_contains(done_rect, e->x, e->y))
            pressed_done = true;
        else if (gfx_rect_contains(p, e->x, e->y))
            pressed_play = true;
        else if (abs(e->y - BAR_Y) < 14 && e->x >= BAR_X0 - 12 && e->x <= BAR_X1 + 12) {
            scrubbing = true;
            scrub_ms = scrub_at(e->x);
        } else {
            overlay = false;
        }
        redraw();
    } else if (e->type == GZ_EV_TOUCH_MOVE) {
        if (scrubbing) {
            scrub_ms = scrub_at(e->x);
            redraw();
        }
    } else if (e->type == GZ_EV_TOUCH_UP) {
        if (pressed_done) {
            pressed_done = false;
            if (gfx_rect_contains(done_rect, e->x, e->y)) {
                leave_player();
                return;
            }
        } else if (pressed_play) {
            pressed_play = false;
            if (gfx_rect_contains(p, e->x, e->y))
                toggle_play();
        } else if (scrubbing) {
            scrubbing = false;
            seek_to(scrub_at(e->x));
        }
        overlay_until = gz_time_ms() + OVERLAY_MS;
        redraw();
    }
}

static void key_event(const struct gz_input_event *e)
{
    if (e->type != GZ_EV_KEY_DOWN || view != V_PLAYER)
        return;
    switch (e->code) {
    case GZ_KEY_OK:
        toggle_play();
        break;
    case GZ_KEY_LEFT:
        seek_to(clock_ms() > SEEK_STEP_MS ? clock_ms() - SEEK_STEP_MS : 0);
        show_overlay();
        break;
    case GZ_KEY_RIGHT:
        seek_to(clock_ms() + SEEK_STEP_MS);
        show_overlay();
        break;
    case GZ_KEY_ESC:
        leave_player();
        return;
    }
    redraw();
}

int main(void)
{
    ui_init(SB_STATUS_BLACK);
    list_videos();
    ui_list_init(&list, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H), ROW_H);
    ui_list_reset(&list, nvideos);
    redraw();

    for (;;) {
        struct gz_input_event ev;
        int wait = -1;

        if (view == V_PLAYER && playing)
            wait = 10;
        else if (view == V_LIST && (next_thumb() >= 0 || list.sc.animating))
            wait = list.sc.animating ? 30 : 0;
        if (ui_next_event(&ev, wait)) {
            if (ev.type == GZ_EV_APP_DEACTIVATE) {
                if (view == V_PLAYER && playing)
                    toggle_play();      /* Home: pause, which also frees the Music player */
            } else if (ev.type == GZ_EV_KEY_DOWN || ev.type == GZ_EV_KEY_UP)
                key_event(&ev);
            else if (view == V_LIST)
                list_event(&ev);
            else
                player_event(&ev);
            continue;
        }
        if (view == V_PLAYER) {
            if (playing)
                play_step();
        } else if (ui_scroll_step(&list.sc)) {
            redraw();
        } else {
            int i = next_thumb();
            if (i >= 0) {
                load_thumb(i);
                redraw();
            }
        }
    }
}
