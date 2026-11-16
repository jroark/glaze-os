/*
 * Clock: World Clock, Stopwatch and Timer tabs.
 *
 * The RTC holds local time only, so world clocks go through UTC in the
 * zone chosen in Settings > Date & Time (glaze/tz.h, with daylight saving).
 */

#include <glaze/audio.h>
#include <glaze/notify.h>
#include <glaze/prefs.h>
#include <glaze/sys.h>
#include <glaze/tz.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <string.h>

enum { TAB_WORLD, TAB_STOPWATCH, TAB_TIMER, NTABS };

static const struct ui_tab tabs[NTABS] = {
    { "World Clock", "tab_world.gzi" },
    { "Stopwatch", "tab_stopwatch.gzi" },
    { "Timer", "tab_timer.gzi" },
};
static const char *const titles[NTABS] = { "World Clock", "Stopwatch", "Timer" };

static int tab;
static int pressed = -1;        /* button id under the finger */

/* ---- world clock -------------------------------------------------------- */

static const char *const cities[] = { "Los Angeles", "New York", "London", "Tokyo" };
#define NCITIES ((int)(sizeof(cities) / sizeof(cities[0])))

static int city_zone[NCITIES];  /* glaze/tz.h zone indices */
static int home_zone;

static void analog_clock(gfx_surface *s, int cx, int cy, int r, int h, int m, int sec)
{
    bool night = h < 6 || h >= 18;
    uint32_t face = night ? 0x1C1C1C : 0xFFFFFF, ink = night ? 0xFFFFFF : 0x111111;

    gfx_circle(s, cx, cy, r + 1, 0x777777, 255);
    gfx_circle(s, cx, cy, r, face, 255);
    for (int i = 0; i < 12; i++) {
        /* Tick positions from a small sine table (30-degree steps). */
        static const int sin30[12] = { 0, 500, 866, 1000, 866, 500, 0, -500, -866, -1000, -866, -500 };
        int sx = sin30[i], sy = -sin30[(i + 3) % 12];
        int r0 = r - (i % 3 ? 3 : 5), r1 = r - 1;
        gfx_line_aa(s, (cx * 16) + sx * r0 * 16 / 1000, (cy * 16) + sy * r0 * 16 / 1000,
                    (cx * 16) + sx * r1 * 16 / 1000, (cy * 16) + sy * r1 * 16 / 1000, i % 3 ? 12 : 24, ink, 255);
    }
    /* Hands: angle in 1/60 turns using a 60-step sine table. */
    static int sin60[60];
    if (!sin60[15]) {
        static const int q[16] = { 0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914, 951, 978, 995, 1000 };
        for (int i = 0; i < 60; i++) {
            int k = i % 30, v = k <= 15 ? q[k] : q[30 - k];
            sin60[i] = i < 30 ? v : -v;
        }
    }
    struct { int pos, len, w; uint32_t col; } hands[3] = {
        { (h % 12) * 5 + m / 12, r * 55 / 100, 40, ink },
        { m, r * 80 / 100, 28, ink },
        { sec, r * 85 / 100, 12, 0xD0201A },
    };
    for (int i = 0; i < 3; i++) {
        int sx = sin60[hands[i].pos % 60], sy = -sin60[(hands[i].pos + 15) % 60];
        gfx_line_aa(s, cx * 16, cy * 16, cx * 16 + sx * hands[i].len * 16 / 1000,
                    cy * 16 + sy * hands[i].len * 16 / 1000, hands[i].w, hands[i].col, 255);
    }
    gfx_circle(s, cx, cy, 2, 0xD0201A, 255);
}

static void draw_world(gfx_surface *s)
{
    struct gz_datetime now;
    const gfx_font *nf = ui_font(FONT_B13), *df = ui_font(FONT_R11), *tf = ui_font(FONT_R24);
    /* A column of 60 px rows; in landscape, two columns of shorter cells
     * with the time under the city. */
    bool land = ui_landscape();
    int y0 = UI_NAV_H, body = ui_h() - y0 - UI_TAB_H, cols = land ? 2 : 1;
    int cell_w = ui_w() / cols, row_h = land ? body / ((NCITIES + 1) / 2) : 60;

    gz_rtc_get(&now);
    gfx_fill(s, gfx_r(0, y0, ui_w(), body), 0x101010);
    for (int i = 0; i < NCITIES; i++) {
        int x = i % cols * cell_w, y = y0 + i / cols * row_h;
        gfx_vgradient(s, gfx_r(x, y, cell_w - (cols > 1 && x == 0), row_h - 1), 0x3B3B3B, 0x1E1E1E, 255);
        gfx_fill(s, gfx_r(x, y + row_h - 1, cell_w, 1), 0x000000);
        if (cols > 1 && x == 0)
            gfx_fill(s, gfx_r(cell_w - 1, y, 1, row_h), 0x000000);
        uint32_t local = gz_dt_to_secs(&now);
        uint32_t there = tz_utc_to_local(city_zone[i], tz_local_to_utc(home_zone, local));
        int day = (int)(there / 86400u) - (int)(local / 86400u);
        int h = (int)(there % 86400u / 3600), m = (int)(there % 3600 / 60);
        analog_clock(s, x + 28, y + row_h / 2, 21, h, m, now.second);
        int ty = land ? (row_h - 66) / 2 : 0;
        gfx_text(s, x + 56, y + ty + 12, nf, cities[i], 0xFFFFFF, 255);
        gfx_text(s, x + 56, y + ty + (land ? 28 : 34), df, day < 0 ? "Yesterday" : day > 0 ? "Tomorrow" : "Today",
                 0xA0A0A0, 255);
        char t[12];
        snprintf(t, sizeof(t), "%d:%02d", h % 12 ? h % 12 : 12, m);
        int tw = gfx_text_width(tf, t);
        int tx = land ? x + 56 : x + cell_w - 36 - tw, tty = land ? y + ty + 40 : y + 14;
        gfx_text(s, tx, tty, tf, t, 0xFFFFFF, 255);
        gfx_text(s, land ? tx + tw + 4 : x + cell_w - 32, tty + 10, df, h < 12 ? "AM" : "PM", 0xC0C0C0, 255);
    }
}

/* ---- stopwatch ---------------------------------------------------------- */

static bool sw_running;
static uint32_t sw_start, sw_accum;        /* ms */
static uint32_t laps[32];
static int nlaps;

static uint32_t sw_elapsed(void)
{
    return sw_accum + (sw_running ? gz_time_ms() - sw_start : 0);
}

static void fmt_sw(uint32_t ms, char *buf, size_t n)
{
    snprintf(buf, n, "%02u:%02u.%u", ms / 60000, ms / 1000 % 60, ms / 100 % 10);
}

enum { B_LEFT = 1, B_RIGHT };

static gfx_rect left_btn, right_btn;

static void round_button(gfx_surface *s, gfx_rect r, const char *label, uint32_t top, uint32_t bottom, bool down)
{
    const gfx_font *f = ui_font(FONT_B17);

    gfx_round_rect(s, r, 8, 0x000000, 255);
    gfx_round_rect_vgradient(s, gfx_r(r.x + 1, r.y + 1, r.w - 2, r.h - 2), 7, top, bottom, 255);
    gfx_round_rect(s, gfx_r(r.x + 1, r.y + 1, r.w - 2, (r.h - 2) / 2), 7, 0xFFFFFF, 40);
    if (down)
        gfx_round_rect(s, r, 8, 0x000000, 90);
    gfx_text_center(s, r.x, r.y + (r.h - f->line_height) / 2, r.w, f, label, 0xFFFFFF, 255);
}

static void draw_stopwatch(gfx_surface *s)
{
    char buf[24];
    int y0 = UI_NAV_H, bottom = ui_h() - UI_TAB_H;
    const gfx_font *big = ui_font(FONT_R44), *lf = ui_font(FONT_R13);
    /* The time and buttons span a portrait screen above the laps; in
     * landscape they make a left panel, with the laps to its right. */
    bool land = ui_landscape();
    int pw = land ? 176 : ui_w(), bw = (pw - 36) / 2;

    gfx_vgradient(s, gfx_r(0, y0, pw, 84), 0xE6E9ED, 0xB9C0C9, 255);
    fmt_sw(sw_elapsed(), buf, sizeof(buf));
    gfx_text_center(s, 0, y0 + 16, pw, big, buf, 0x111111, 255);

    gfx_fill(s, gfx_r(0, y0 + 84, pw, land ? bottom - y0 - 84 : 52), 0x2A2A2A);
    left_btn = gfx_r(12, y0 + 92, bw, 36);
    right_btn = gfx_r(24 + bw, y0 + 92, bw, 36);
    if (sw_running)
        round_button(s, left_btn, "Stop", 0xF06A5E, 0xB51D15, pressed == B_LEFT);
    else
        round_button(s, left_btn, "Start", 0x6BD267, 0x22891E, pressed == B_LEFT);
    round_button(s, right_btn, sw_running ? "Lap" : "Reset", 0x8A8A8A, 0x4A4A4A, pressed == B_RIGHT);

    int lx = land ? pw : 0, ly = land ? y0 : y0 + 136, lw = ui_w() - lx, lh = 22;
    gfx_fill(s, gfx_r(lx, ly, lw, bottom - ly), 0xFFFFFF);
    for (int i = 0; ly + (i + 1) * lh <= bottom; i++) {
        int y = ly + i * lh;
        gfx_fill(s, gfx_r(lx, y + lh - 1, lw, 1), 0xDDDDDD);
        int lap = nlaps - 1 - i;
        if (lap < 0)
            continue;
        char name[16];
        snprintf(name, sizeof(name), "Lap %d", lap + 1);
        gfx_text(s, lx + 12, y + 4, lf, name, 0x333333, 255);
        fmt_sw(laps[lap] - (lap ? laps[lap - 1] : 0), buf, sizeof(buf));
        gfx_text(s, ui_w() - 12 - gfx_text_width(lf, buf), y + 4, lf, buf, 0x111111, 255);
    }
}

static void stopwatch_press(int b)
{
    if (b == B_LEFT) {
        if (sw_running) {
            sw_accum = sw_elapsed();
            sw_running = false;
        } else {
            sw_start = gz_time_ms();
            sw_running = true;
        }
    } else if (b == B_RIGHT) {
        if (sw_running) {
            if (nlaps < 32)
                laps[nlaps++] = sw_elapsed();
        } else {
            sw_accum = 0;
            nlaps = 0;
        }
    }
}

/* ---- timer -------------------------------------------------------------- */

static int tm_hours, tm_minutes = 5;
/* The timer ends at a wall-clock time (ui_now_secs) and is also posted to
 * SpringBoard as notification TIMER_NOTE, which fires it after Clock has
 * gone and lets a relaunched Clock pick the countdown up again. */
#define TIMER_NOTE  1

static bool tm_running, tm_done;
static uint32_t tm_end;
static gfx_rect step_rects[4], alert_ok;

enum { B_H_DOWN = 10, B_H_UP, B_M_DOWN, B_M_UP, B_ALERT };

static void stepper(gfx_surface *s, gfx_rect r, const char *label, int id)
{
    const gfx_font *f = ui_font(FONT_B20);

    gfx_round_rect(s, r, 6, 0x000000, 255);
    gfx_round_rect_vgradient(s, gfx_r(r.x + 1, r.y + 1, r.w - 2, r.h - 2), 5,
                             pressed == id ? 0x4A4A4A : 0x8A8A8A, 0x3A3A3A, 255);
    gfx_text_center(s, r.x, r.y + (r.h - f->line_height) / 2, r.w, f, label, 0xFFFFFF, 255);
}

static void draw_timer(gfx_surface *s)
{
    char buf[24];
    int y0 = UI_NAV_H, bottom = ui_h() - UI_TAB_H;
    const gfx_font *big = ui_font(FONT_R44), *lf = ui_font(FONT_B11);
    /* Landscape: the picker moves up and left, Start goes to its right. */
    bool land = ui_landscape();
    int dx = land ? -8 : 0, dy = land ? -20 : 0, pw = land ? 220 : ui_w();

    gfx_vgradient(s, gfx_r(0, y0, ui_w(), bottom - y0), 0x3A3A3A, 0x121212, 255);
    if (tm_running) {
        int32_t left = (int32_t)(tm_end - ui_now_secs());
        uint32_t secs = left > 0 ? (uint32_t)left : 0;
        snprintf(buf, sizeof(buf), "%u:%02u:%02u", secs / 3600, secs / 60 % 60, secs % 60);
        gfx_text_center(s, 0, y0 + 50, pw, big, buf, 0xFFFFFF, 255);
    } else {
        /* Hours and minutes columns with steppers, standing in for the
         * original's picker wheels. */
        int top = y0 + dy;
        gfx_round_rect(s, gfx_r(dx + 20, top + 30, 200, 96), 8, 0xFFFFFF, 255);
        gfx_round_rect(s, gfx_r(dx + 20, top + 30, 200, 48), 8, 0xDDDDDD, 140);
        snprintf(buf, sizeof(buf), "%d", tm_hours);
        gfx_text_center(s, dx + 20, top + 48, 70, big, buf, 0x111111, 255);
        gfx_text(s, dx + 88, top + 76, lf, "hours", 0x333333, 255);
        snprintf(buf, sizeof(buf), "%d", tm_minutes);
        gfx_text_center(s, dx + 120, top + 48, 70, big, buf, 0x111111, 255);
        gfx_text(s, dx + 186, top + 76, lf, "min", 0x333333, 255);
        int sy = land ? top + 132 : top + 136;
        step_rects[0] = gfx_r(dx + 28, sy, 40, 30);
        step_rects[1] = gfx_r(dx + 72, sy, 40, 30);
        step_rects[2] = gfx_r(dx + 128, sy, 40, 30);
        step_rects[3] = gfx_r(dx + 172, sy, 40, 30);
        stepper(s, step_rects[0], GFX_CH_MINUS, B_H_DOWN);
        stepper(s, step_rects[1], "+", B_H_UP);
        stepper(s, step_rects[2], GFX_CH_MINUS, B_M_DOWN);
        stepper(s, step_rects[3], "+", B_M_UP);
    }
    left_btn = land ? gfx_r(pw + 6, y0 + (bottom - y0 - 38) / 2, ui_w() - pw - 14, 38)
                    : gfx_r(30, bottom - 52, 180, 38);
    if (tm_running)
        round_button(s, left_btn, "Cancel", 0xF06A5E, 0xB51D15, pressed == B_LEFT);
    else
        round_button(s, left_btn, "Start", 0x6BD267, 0x22891E, pressed == B_LEFT);
    if (tm_done) {
        gfx_rect unused;
        ui_draw_alert(s, "Timer Done", NULL, "OK", NULL, &alert_ok, &unused, pressed == B_ALERT ? 0 : -1);
    }
}

static void timer_press(int b)
{
    switch (b) {
    case B_LEFT:
        if (tm_running) {
            tm_running = false;
            ui_notify_cancel(TIMER_NOTE);
        } else if (tm_hours || tm_minutes) {
            tm_end = ui_now_secs() + (uint32_t)(tm_hours * 3600 + tm_minutes * 60);
            tm_running = true;
            ui_notify_post(TIMER_NOTE, tm_end, "Timer Done", "Clock", 1);
        }
        break;
    case B_H_DOWN: tm_hours = (tm_hours + 23) % 24; break;
    case B_H_UP: tm_hours = (tm_hours + 1) % 24; break;
    case B_M_DOWN: tm_minutes = (tm_minutes + 59) % 60; break;
    case B_M_UP: tm_minutes = (tm_minutes + 1) % 60; break;
    case B_ALERT:
        tm_done = false;
        audio_sound_stop();
        break;
    }
}

/* ---- shell -------------------------------------------------------------- */

static void draw(void)
{
    gfx_surface *s = &ui.surf;

    switch (tab) {
    case TAB_WORLD: draw_world(s); break;
    case TAB_STOPWATCH: draw_stopwatch(s); break;
    case TAB_TIMER: draw_timer(s); break;
    }
    ui_draw_navbar(s, titles[tab], NULL, NULL, 1);
    ui_draw_tabbar(s, tabs, NTABS, tab);
    ui_invalidate_all();
    ui_present();
}

static int hit(int x, int y)
{
    if (tab == TAB_TIMER && tm_done)
        return gfx_rect_contains(alert_ok, x, y) ? B_ALERT : -1;
    if ((tab == TAB_STOPWATCH || tab == TAB_TIMER) && gfx_rect_contains(left_btn, x, y))
        return B_LEFT;
    if (tab == TAB_STOPWATCH && gfx_rect_contains(right_btn, x, y))
        return B_RIGHT;
    if (tab == TAB_TIMER && !tm_running) {
        for (int i = 0; i < 4; i++) {
            if (gfx_rect_contains(step_rects[i], x, y))
                return B_H_DOWN + i;
        }
    }
    return -1;
}

int main(void)
{
    ui_init(SB_STATUS_BLACK);
    ui_set_orientations(SB_ORIENT_PORTRAIT | SB_ORIENT_LANDSCAPE);
    home_zone = tz_current();
    for (int i = 0; i < NCITIES; i++) {
        for (int z = 0; z < tz_count(); z++) {
            if (!strcmp(tz_get(z)->name, cities[i]))
                city_zone[i] = z;
        }
    }
    ui_set_badge(0);
    if ((tm_end = ui_notify_pending(TIMER_NOTE)) != 0) {
        tm_running = true;
        tab = TAB_TIMER;
    }
    draw();

    bool rung = false;
    for (;;) {
        int timeout = 1000;
        if ((tab == TAB_STOPWATCH && sw_running) || (tab == TAB_TIMER && tm_running))
            timeout = 100;

        struct gz_input_event ev;
        if (ui_next_event(&ev, timeout)) {
            if (ev.type == GZ_EV_TOUCH_DOWN) {
                int t = tm_done ? -1 : ui_tabbar_hit(NTABS, ev.x, ev.y);
                if (t >= 0) {
                    tab = t;
                    pressed = -1;
                } else {
                    pressed = hit(ev.x, ev.y);
                }
            } else if (ev.type == GZ_EV_TOUCH_UP) {
                if (pressed >= 0 && hit(ev.x, ev.y) == pressed) {
                    if (tab == TAB_STOPWATCH)
                        stopwatch_press(pressed);
                    else if (tab == TAB_TIMER)
                        timer_press(pressed);
                }
                pressed = -1;
            } else if (ev.type == GZ_EV_TOUCH_CANCEL) {
                pressed = -1;                   /* Home mid-touch: not a press */
            } else if (ev.type == GZ_EV_APP_RESIZE) {
                pressed = -1;                   /* laid out again as it draws */
            } else if (ev.type == GZ_EV_APP_NOTIFY && ev.code == TIMER_NOTE && tm_running) {
                tm_end = ui_now_secs();         /* SpringBoard's clock got there first */
                rung = true;                    /* and it rang the timer */
            } else {
                continue;
            }
        }
        if (tm_running && (int32_t)(tm_end - ui_now_secs()) <= 0) {
            tm_running = false;
            tm_done = true;
            tab = TAB_TIMER;
            ui_notify_cancel(TIMER_NOTE);       /* in front: no banner or badge */
            if (!rung)
                audio_sound(AUDIO_SND_TIMER);   /* rings until OK */
            rung = false;
        }
        draw();
    }
}
