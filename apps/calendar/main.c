/* Calendar: month grid with today highlighted; arrows change the month. */

#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <string.h>

static const char *const months[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
};
static const char *const wd_short[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const wd_long[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};

static int year, month;                 /* shown month, 1-based */
static struct gz_datetime today;
static int sel_day;                     /* selected day in the shown month, 0 = none */
static int pressed = 0;                 /* 1 prev, 2 next */

#define HEADER_Y    UI_NAV_H
#define HEADER_H    44
#define CELL_W      34
#define CELL_H      30
#define GRID_X      1
#define GRID_Y      (HEADER_Y + HEADER_H)

static bool leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int days_in(int y, int m)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return d[m - 1] + (m == 2 && leap(y));
}

/* Day of week of y-m-1 (Sakamoto), 0 = Sunday. */
static int first_weekday(int y, int m)
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3)
        y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + 1) % 7;
}

static gfx_rect prev_rect = { 0, HEADER_Y, 48, 26 }, next_rect = { UI_W - 48, HEADER_Y, 48, 26 };

static gfx_rect cell_rect(int index)
{
    return gfx_r(GRID_X + (index % 7) * CELL_W, GRID_Y + (index / 7) * CELL_H, CELL_W, CELL_H);
}

static void draw(void)
{
    gfx_surface *s = &ui.surf;
    char buf[48];
    const gfx_font *tf = ui_font(FONT_B17), *wf = ui_font(FONT_B8), *df = ui_font(FONT_B14);

    ui_draw_navbar(s, "Calendar", NULL, NULL, 0);
    gfx_vgradient(s, gfx_r(0, HEADER_Y, UI_W, HEADER_H), 0xF7F7F7, 0xCDD1D7, 255);
    snprintf(buf, sizeof(buf), "%s %d", months[month - 1], year);
    gfx_text_center(s, 0, HEADER_Y + 5, UI_W, tf, buf, 0x2C3138, 255);
    /* Month arrows (filled triangles). */
    for (int side = 0; side < 2; side++) {
        int cx = side ? UI_W - 24 : 24, cy = HEADER_Y + 14;
        uint32_t c = pressed == side + 1 ? 0x2A63D4 : 0x4A5260;
        for (int i = 0; i < 7; i++) {
            int x = side ? cx + 3 - i : cx - 3 + i;
            gfx_fill(s, gfx_r(x, cy - i, 1, i * 2 + 1), c);
        }
    }
    for (int d = 0; d < 7; d++)
        gfx_text_center(s, GRID_X + d * CELL_W, HEADER_Y + 31, CELL_W, wf, wd_short[d], 0x4A5260, 255);

    int first = first_weekday(year, month), n = days_in(year, month);
    int pm = month == 1 ? 12 : month - 1, py = month == 1 ? year - 1 : year;
    int prev_n = days_in(py, pm);
    for (int i = 0; i < 42; i++) {
        gfx_rect r = cell_rect(i);
        int day = i - first + 1;
        bool in_month = day >= 1 && day <= n;
        bool is_today = in_month && year == today.year && month == today.month && day == today.day;
        bool is_sel = in_month && day == sel_day;
        int shown = in_month ? day : day < 1 ? prev_n + day : day - n;

        if (is_today)
            gfx_vgradient(s, r, 0x7FA3D6, 0x2E5DAA, 255);
        else if (is_sel)
            gfx_vgradient(s, r, 0x93A3B8, 0x5E6E85, 255);
        else
            gfx_vgradient(s, r, in_month ? 0xE4E6EA : 0xCCCFD4, in_month ? 0xD5D8DD : 0xC2C5CB, 255);
        gfx_fill(s, gfx_r(r.x, r.y, r.w, 1), 0xFFFFFF);
        gfx_fill(s, gfx_r(r.x + r.w - 1, r.y, 1, r.h), 0xA6AAB2);
        gfx_fill(s, gfx_r(r.x, r.y + r.h - 1, r.w, 1), 0xA6AAB2);
        snprintf(buf, sizeof(buf), "%d", shown);
        uint32_t col = is_today || is_sel ? 0xFFFFFF : in_month ? 0x2C3138 : 0x8A9099;
        gfx_text_center(s, r.x, r.y + (r.h - df->line_height) / 2, r.w, df, buf, col, 255);
    }

    int below = GRID_Y + 6 * CELL_H;
    gfx_fill(s, gfx_r(0, below, UI_W, UI_H - below), 0xFFFFFF);
    gfx_fill(s, gfx_r(0, below, UI_W, 1), 0x8A9099);
    int d = sel_day ? sel_day : (year == today.year && month == today.month ? today.day : 1);
    snprintf(buf, sizeof(buf), "%s, %s %d", wd_long[(first + d - 1) % 7], months[month - 1], d);
    gfx_text(s, 10, below + 8, ui_font(FONT_B13), buf, 0x2C3138, 255);
    gfx_text(s, 10, below + 28, ui_font(FONT_R12), "No Events", 0x8A9099, 255);
    ui_invalidate_all();
    ui_present();
}

static void change_month(int delta)
{
    month += delta;
    if (month < 1) {
        month = 12;
        year--;
    } else if (month > 12) {
        month = 1;
        year++;
    }
    sel_day = 0;
}

int main(void)
{
    ui_init(SB_STATUS_GRAY);
    gz_rtc_get(&today);
    year = today.year;
    month = today.month;
    draw();

    for (;;) {
        struct gz_input_event ev;
        if (!ui_next_event(&ev, 60000)) {
            gz_rtc_get(&today);         /* roll over at midnight */
            draw();
            continue;
        }
        if (ev.type == GZ_EV_KEY_DOWN && (ev.code == GZ_KEY_LEFT || ev.code == GZ_KEY_RIGHT)) {
            change_month(ev.code == GZ_KEY_LEFT ? -1 : 1);
            draw();
        } else if (ev.type == GZ_EV_TOUCH_DOWN) {
            pressed = gfx_rect_contains(prev_rect, ev.x, ev.y) ? 1 : gfx_rect_contains(next_rect, ev.x, ev.y) ? 2 : 0;
            draw();
        } else if (ev.type == GZ_EV_TOUCH_UP) {
            if (pressed) {
                change_month(pressed == 1 ? -1 : 1);
                pressed = 0;
            } else {
                for (int i = 0; i < 42; i++) {
                    int day = i - first_weekday(year, month) + 1;
                    if (gfx_rect_contains(cell_rect(i), ev.x, ev.y) && day >= 1 && day <= days_in(year, month))
                        sel_day = day;
                }
            }
            draw();
        }
    }
}
