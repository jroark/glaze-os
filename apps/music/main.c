/*
 * Music: the songs in /Music and a Now Playing screen, in the style of the
 * original iPod app.
 *
 * Playback belongs to the audio server (glaze/audio.h), which keeps playing
 * after the Home button closes this app and moves on to the next song by
 * itself. The app lists files, sends commands and, while it is shown,
 * polls the player's state to move the scrubber.
 */

#include <glaze/audio.h>
#include <glaze/fs.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MUSIC       "/Music"
#define MAX_SONGS   64
#define ROW_H       44
#define POLL_MS     250

/* Now Playing layout. */
#define ART         150
#define ART_Y       (UI_NAV_H + 10)
#define TITLE_Y     (ART_Y + ART + 8)
#define BAR_Y       (TITLE_Y + 44)
#define BAR_X0      44
#define BAR_X1      (UI_W - 44)
#define CTRL_Y      (BAR_Y + 18)
#define CTRL_H      (UI_H - CTRL_Y)

struct song {
    char name[FS_NAME_MAX];
    char title[FS_NAME_MAX];        /* the name without its extension */
};

static struct song songs[MAX_SONGS];
static int nsongs;

enum view { V_LIST, V_NOW };
static enum view view = V_LIST;
static struct audio_state st;
static struct ui_scroll list_scroll;
static int pressed_row = -1;
static bool pressed_now;
static gfx_rect now_rect, back_rect;
static int pressed_ctrl = -1;       /* 0 previous, 1 play/pause, 2 next */
static bool scrubbing;
static uint32_t scrub_ms;

/* ---- songs ------------------------------------------------------------- */

static int song_cmp(const void *a, const void *b)
{
    return strcasecmp(((const struct song *)a)->name, ((const struct song *)b)->name);
}

static void list_songs(void)
{
    struct fs_dirent ents[8];
    uint32_t skip = 0;
    int n;

    while (nsongs < MAX_SONGS && (n = fs_list(MUSIC, skip, ents, 8)) > 0) {
        for (int i = 0; i < n && nsongs < MAX_SONGS; i++) {
            if (ents[i].type != FS_FILE || !audio_is_playable(ents[i].name))
                continue;
            struct song *s = &songs[nsongs++];
            memcpy(s->name, ents[i].name, sizeof(s->name));
            memcpy(s->title, ents[i].name, sizeof(s->title));
            char *dot = strrchr(s->title, '.');
            if (dot && dot != s->title)
                *dot = 0;
        }
        skip += (uint32_t)n;
    }
    qsort(songs, (size_t)nsongs, sizeof(songs[0]), song_cmp);
}

static bool is_current(int i)
{
    const char *slash = strrchr(st.path, '/');

    return st.path[0] && slash && !strcmp(slash + 1, songs[i].name) &&
           !strncmp(st.path, MUSIC "/", sizeof(MUSIC));
}

static bool loaded(void)
{
    return st.path[0] != 0;
}

/* ---- shapes -------------------------------------------------------------- */

/* A triangle pointing right (dir 1) or left (dir -1) in a w x h box. */
static void triangle(gfx_surface *s, int x, int y, int w, int h, int dir, uint32_t rgb)
{
    for (int i = 0; i < w; i++) {
        int inset = i * h / (2 * w);
        int col = dir > 0 ? x + i : x + w - 1 - i;
        gfx_fill(s, gfx_r(col, y + inset, 1, h - 2 * inset), rgb);
    }
}

static void draw_play_pause(gfx_surface *s, int cx, int cy, bool playing, uint32_t rgb)
{
    if (playing) {
        gfx_fill(s, gfx_r(cx - 7, cy - 9, 5, 18), rgb);
        gfx_fill(s, gfx_r(cx + 2, cy - 9, 5, 18), rgb);
    } else {
        triangle(s, cx - 7, cy - 10, 17, 20, 1, rgb);
    }
}

static void draw_skip(gfx_surface *s, int cx, int cy, int dir, uint32_t rgb)
{
    int x = dir > 0 ? cx - 11 : cx - 8;

    triangle(s, x, cy - 7, 10, 14, dir, rgb);
    triangle(s, x + 9, cy - 7, 10, 14, dir, rgb);
    gfx_fill(s, gfx_r(dir > 0 ? cx + 8 : cx - 11, cy - 7, 3, 14), rgb);
}

/* A beamed pair of eighth notes, as on the icon, `u` pixels per unit. */
static void draw_note(gfx_surface *s, int x, int y, int u, uint32_t rgb)
{
    gfx_circle(s, x + 5 * u, y + 29 * u, 5 * u, rgb, 255);
    gfx_circle(s, x + 21 * u, y + 26 * u, 5 * u, rgb, 255);
    gfx_fill(s, gfx_r(x + 8 * u, y + 7 * u, 3 * u, 22 * u), rgb);
    gfx_fill(s, gfx_r(x + 24 * u, y + 4 * u, 3 * u, 22 * u), rgb);
    for (int i = 0; i < 19 * u; i++) {
        int top = y + 7 * u - i * 3 / 19;
        gfx_fill(s, gfx_r(x + 8 * u + i, top, 1, 5 * u), rgb);
    }
}

static void fmt_time(char *buf, size_t n, uint32_t ms, bool minus)
{
    uint32_t sec = ms / 1000;

    snprintf(buf, n, "%s%u:%02u", minus ? "-" : "", sec / 60, sec % 60);
}

/* ---- list ------------------------------------------------------------------ */

static int row_y(int i)
{
    return UI_NAV_H + i * ROW_H - list_scroll.offset;
}

static void draw_list(gfx_surface *s)
{
    gfx_rect body = gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H);
    const gfx_font *f = ui_font(FONT_B17), *sf = ui_font(FONT_R14);

    gfx_set_clip(s, body);
    gfx_fill(s, body, 0xFFFFFF);
    for (int i = 0; i < nsongs; i++) {
        int y = row_y(i);
        if (y + ROW_H <= body.y || y >= body.y + body.h)
            continue;
        bool sel = i == pressed_row;
        if (sel)
            gfx_vgradient(s, gfx_r(0, y, UI_W, ROW_H - 1), UI_SELECT_TOP, UI_SELECT_BOTTOM, 255);
        gfx_fill(s, gfx_r(0, y + ROW_H - 1, UI_W, 1), 0xD9D9D9);
        int x = 10;
        if (is_current(i)) {
            /* The speaker mark of the original: a blue play arrow. */
            triangle(s, 8, y + ROW_H / 2 - 6, 10, 12, 1, sel ? 0xFFFFFF : UI_BLUE_BUTTON);
            x = 24;
        }
        gfx_text_fit(s, x, y + (ROW_H - f->line_height) / 2, UI_W - x - 10, f, songs[i].title,
                     sel ? 0xFFFFFF : UI_TEXT, 255);
    }
    if (nsongs) {
        char count[24];
        snprintf(count, sizeof(count), nsongs == 1 ? "%d Song" : "%d Songs", nsongs);
        gfx_text_center(s, 0, row_y(nsongs) + 12, UI_W, sf, count, 0x808080, 255);
    } else {
        gfx_text_center(s, 0, UI_NAV_H + 100, UI_W, ui_font(FONT_B17), "No Songs", 0x666666, 255);
        gfx_text_center(s, 0, UI_NAV_H + 126, UI_W, sf, "MP3 and WAV files you add", 0x888888, 255);
        gfx_text_center(s, 0, UI_NAV_H + 144, UI_W, sf, "to " MUSIC " appear here.", 0x888888, 255);
    }
    gfx_reset_clip(s);
    ui_draw_navbar(s, "Songs", NULL, NULL, 0);
    now_rect = loaded() ? ui_draw_bar_button(s, "Now Playing", true, pressed_now, false, 0) : gfx_r(0, 0, 0, 0);
}

/* ---- Now Playing ----------------------------------------------------------- */

static uint32_t shown_position(void)
{
    return scrubbing ? scrub_ms : st.position_ms;
}

static int knob_x(void)
{
    if (!st.duration_ms)
        return BAR_X0;
    return BAR_X0 + (int)((uint64_t)MIN(shown_position(), st.duration_ms) * (BAR_X1 - BAR_X0) / st.duration_ms);
}

static gfx_rect ctrl_rect(int i)
{
    return gfx_r(30 + i * 60, CTRL_Y, 60, CTRL_H);
}

static void draw_now(gfx_surface *s)
{
    const gfx_font *tf = ui_font(FONT_B17), *af = ui_font(FONT_R13), *nf = ui_font(FONT_B11);
    gfx_rect art = gfx_r((UI_W - ART) / 2, ART_Y, ART, ART);
    char a[16], b[16];

    gfx_fill(s, gfx_r(0, 0, UI_W, UI_H), 0x000000);
    /* No cover art in the files: a plain sleeve with a note on it. */
    gfx_vgradient(s, art, 0x5C6168, 0x1C1E21, 255);
    gfx_fill(s, gfx_r(art.x, art.y, art.w, 1), 0x8A9098);
    draw_note(s, art.x + 37, art.y + 30, 3, 0x9EA4AC);
    gfx_vgradient(s, gfx_r(art.x, art.y + art.h, art.w, 6), 0x2A2D30, 0x000000, 255);

    if (gfx_text_width(tf, st.title) <= UI_W - 20)
        gfx_text_center(s, 0, TITLE_Y, UI_W, tf, st.title, 0xFFFFFF, 255);
    else
        gfx_text_fit(s, 10, TITLE_Y, UI_W - 20, tf, st.title, 0xFFFFFF, 255);
    gfx_text_center(s, 0, TITLE_Y + 22, UI_W, af, st.artist[0] ? st.artist : "Unknown Artist", 0xA0A0A0, 255);

    /* Scrubber: elapsed, track, remaining. */
    uint32_t pos = MIN(shown_position(), st.duration_ms);
    fmt_time(a, sizeof(a), pos, false);
    fmt_time(b, sizeof(b), st.duration_ms - pos, true);
    gfx_text(s, 6, BAR_Y - nf->line_height / 2, nf, a, 0xFFFFFF, 255);
    gfx_text(s, UI_W - 6 - gfx_text_width(nf, b), BAR_Y - nf->line_height / 2, nf, b, 0xFFFFFF, 255);
    int kx = knob_x();
    gfx_round_rect(s, gfx_r(BAR_X0, BAR_Y - 2, BAR_X1 - BAR_X0, 5), 2, 0x4A4A4A, 255);
    gfx_round_rect(s, gfx_r(BAR_X0, BAR_Y - 2, kx - BAR_X0 + 2, 5), 2, 0xD8D8D8, 255);
    gfx_circle(s, kx, BAR_Y, scrubbing ? 8 : 6, 0xFFFFFF, 255);
    gfx_circle(s, kx, BAR_Y, scrubbing ? 6 : 4, 0xD0D0D0, 255);

    /* Controls on a dark glass strip. */
    gfx_vgradient(s, gfx_r(0, CTRL_Y, UI_W, CTRL_H), 0x2E2E2E, 0x0A0A0A, 255);
    gfx_fill(s, gfx_r(0, CTRL_Y, UI_W, 1), 0x4A4A4A);
    for (int i = 0; i < 3; i++) {
        gfx_rect r = ctrl_rect(i);
        int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        if (i == pressed_ctrl)
            gfx_circle(s, cx, cy, 18, 0xFFFFFF, 60);
        if (i == 1)
            draw_play_pause(s, cx, cy, st.state == AUDIO_PLAYING, 0xFFFFFF);
        else
            draw_skip(s, cx, cy, i == 0 ? -1 : 1, 0xFFFFFF);
    }
    ui_draw_navbar(s, "Now Playing", "Songs", &back_rect, 1);
}

static void draw_current(gfx_surface *s)
{
    if (view == V_LIST)
        draw_list(s);
    else
        draw_now(s);
}

static void redraw(void)
{
    draw_current(&ui.surf);
    ui_invalidate_all();
    ui_present();
}

/* Re-reads the player state; true if anything shown changed. */
static bool poll_state(void)
{
    struct audio_state old = st;

    if (audio_get_state(&st) < 0)
        memset(&st, 0, sizeof(st));
    return old.state != st.state || old.position_ms / 1000 != st.position_ms / 1000 ||
           old.duration_ms != st.duration_ms || strcmp(old.path, st.path) || strcmp(old.title, st.title);
}

/* ---- events ------------------------------------------------------------------ */

static void show_now(void)
{
    view = V_NOW;
    ui_transition(draw_current, true);
}

static void show_list(void)
{
    view = V_LIST;
    ui_transition(draw_current, false);
}

static void play_song(int i)
{
    char path[FS_PATH_MAX];

    snprintf(path, sizeof(path), MUSIC "/%s", songs[i].name);
    audio_play(path);
    poll_state();
    show_now();
}

static int row_hit(int y)
{
    if (y < UI_NAV_H)
        return -1;
    int i = (y - UI_NAV_H + list_scroll.offset) / ROW_H;
    return i < nsongs ? i : -1;
}

static void list_event(const struct gz_input_event *e)
{
    if (e->type == GZ_EV_TOUCH_DOWN) {
        if (e->y < UI_NAV_H) {
            pressed_now = gfx_rect_contains(now_rect, e->x, e->y);
            if (pressed_now)
                redraw();
            return;
        }
        ui_scroll_event(&list_scroll, e, e->y - UI_NAV_H);
        pressed_row = row_hit(e->y);
        redraw();
    } else if (e->type == GZ_EV_TOUCH_MOVE) {
        if (pressed_now)
            return;
        if (ui_scroll_event(&list_scroll, e, e->y - UI_NAV_H)) {
            if (list_scroll.moved)
                pressed_row = -1;
            redraw();
        }
    } else if (e->type == GZ_EV_TOUCH_UP) {
        if (pressed_now) {
            pressed_now = false;
            if (gfx_rect_contains(now_rect, e->x, e->y))
                show_now();
            else
                redraw();
            return;
        }
        ui_scroll_event(&list_scroll, e, e->y - UI_NAV_H);
        int i = pressed_row;
        pressed_row = -1;
        if (i >= 0 && !list_scroll.moved && row_hit(e->y) == i)
            play_song(i);
        else
            redraw();
    }
}

static uint32_t scrub_at(int x)
{
    x = MAX(BAR_X0, MIN(BAR_X1, x));
    return (uint32_t)((uint64_t)(x - BAR_X0) * st.duration_ms / (BAR_X1 - BAR_X0));
}

static void control(int i)
{
    if (i == 0)
        audio_prev();
    else if (i == 2)
        audio_next();
    else if (st.state == AUDIO_PLAYING)
        audio_pause();
    else
        audio_resume();
    poll_state();
}

static void now_event(const struct gz_input_event *e)
{
    if (e->type == GZ_EV_TOUCH_DOWN) {
        if (e->y < UI_NAV_H) {
            if (gfx_rect_contains(back_rect, e->x, e->y))
                show_list();
            return;
        }
        if (abs(e->y - BAR_Y) < 14 && e->x >= BAR_X0 - 12 && e->x <= BAR_X1 + 12 && st.duration_ms) {
            scrubbing = true;
            scrub_ms = scrub_at(e->x);
        } else {
            for (int i = 0; i < 3; i++)
                if (gfx_rect_contains(ctrl_rect(i), e->x, e->y))
                    pressed_ctrl = i;
        }
        redraw();
    } else if (e->type == GZ_EV_TOUCH_MOVE) {
        if (scrubbing) {
            scrub_ms = scrub_at(e->x);
            redraw();
        }
    } else if (e->type == GZ_EV_TOUCH_UP) {
        if (scrubbing) {
            scrubbing = false;
            audio_seek(scrub_at(e->x));
            poll_state();
        } else if (pressed_ctrl >= 0) {
            int i = pressed_ctrl;
            pressed_ctrl = -1;
            if (gfx_rect_contains(ctrl_rect(i), e->x, e->y))
                control(i);
        }
        redraw();
    }
}

static void key_event(const struct gz_input_event *e)
{
    if (e->type != GZ_EV_KEY_DOWN)
        return;
    switch (e->code) {
    case GZ_KEY_OK:
        if (loaded()) {
            control(1);
            redraw();
        }
        break;
    case GZ_KEY_LEFT:
    case GZ_KEY_RIGHT:
        if (loaded()) {
            control(e->code == GZ_KEY_LEFT ? 0 : 2);
            redraw();
        }
        break;
    case GZ_KEY_ESC:
        if (view == V_NOW)
            show_list();
        break;
    }
}

int main(void)
{
    ui_init(SB_STATUS_BLACK);
    list_songs();
    ui_scroll_reset(&list_scroll, nsongs * ROW_H + 40, UI_H - UI_NAV_H);
    poll_state();
    if (loaded() && st.state != AUDIO_STOPPED)
        view = V_NOW;
    redraw();

    uint32_t last_poll = gz_time_ms();
    for (;;) {
        struct gz_input_event ev;
        int wait = list_scroll.animating && view == V_LIST ? 30 : POLL_MS;

        if (ui_next_event(&ev, wait)) {
            if (ev.type == GZ_EV_KEY_DOWN || ev.type == GZ_EV_KEY_UP)
                key_event(&ev);
            else if (view == V_LIST)
                list_event(&ev);
            else
                now_event(&ev);
        } else if (view == V_LIST && ui_scroll_step(&list_scroll)) {
            redraw();
        }
        if (gz_time_ms() - last_poll >= POLL_MS) {
            last_poll = gz_time_ms();
            if (poll_state() && !scrubbing)
                redraw();
        }
    }
}
