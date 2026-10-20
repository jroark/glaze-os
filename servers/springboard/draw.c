/*
 * SpringBoard rendering: status bar, lock screen, home screen. Pure
 * functions of struct sb_view so the host preview can render them too.
 */

#include "sb_internal.h"

#include <glaze/res.h>
#include <stdio.h>
#include <string.h>

static const struct sb_app_info builtin_apps[] = {
    { .label = "Calendar", .icon = "icon_calendar.gzi", .path = "bin/calendar", .status_style = SB_STATUS_GRAY },
    { .label = "Photos", .icon = "icon_photos.gzi", .path = "bin/photos", .status_style = SB_STATUS_BLACK },
    { .label = "Clock", .icon = "icon_clock.gzi", .path = "bin/clock", .status_style = SB_STATUS_BLACK },
    { .label = "Calculator", .icon = "icon_calculator.gzi", .path = "bin/calculator", .status_style = SB_STATUS_BLACK },
    { .label = "Files", .icon = "icon_files.gzi", .path = "bin/files", .status_style = SB_STATUS_GRAY },
    { .label = "Notes", .icon = "icon_notes.gzi", .path = "bin/notes", .status_style = SB_STATUS_GRAY, .dock = true },
    { .label = "Settings", .icon = "icon_settings.gzi", .path = "bin/settings", .status_style = SB_STATUS_GRAY,
      .dock = true },
    { .label = "Browser", .icon = "icon_web.gzi", .path = "bin/browser", .status_style = SB_STATUS_GRAY },
    { .label = "Music", .icon = "icon_music.gzi", .path = "bin/music", .status_style = SB_STATUS_BLACK },
    { .label = "Videos", .icon = "icon_video.gzi", .path = "bin/video", .status_style = SB_STATUS_BLACK },
    { .label = "App Store", .icon = "icon_appstore.gzi", .path = "bin/appstore", .status_style = SB_STATUS_GRAY },
};
struct sb_app_info sb_apps[SB_MAX_APPS];
int sb_num_apps;

const char *const sb_wallpapers[] = { "wall_aqua.gzi", "wall_dusk.gzi", "wall_meadow.gzi" };
const int sb_num_wallpapers = 3;
bool sb_24h;

static gfx_image icons[SB_MAX_APPS];
static bool icon_ok[SB_MAX_APPS];
static gfx_image walls[3];
static gfx_image knob;
static bool walls_ok[3];

static const char *const weekdays[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};
static const char *const months[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
};

void sb_draw_init(void)
{
    sb_num_apps = (int)(sizeof(builtin_apps) / sizeof(builtin_apps[0]));
    for (int i = 0; i < sb_num_apps; i++) {
        sb_apps[i] = builtin_apps[i];
        sb_apps[i].present = true;
        icon_ok[i] = res_image(&icons[i], sb_apps[i].icon);
    }
    for (int i = 0; i < sb_num_wallpapers; i++)
        walls_ok[i] = res_image(&walls[i], sb_wallpapers[i]);
    res_image(&knob, "ui_knob.gzi");
}

void sb_set_icon(int app, const gfx_image *img)
{
    icon_ok[app] = img != NULL;
    if (img)
        icons[app] = *img;
}

void sb_clock_from(struct sb_clock *c, const struct gz_datetime *dt)
{
    int h12 = dt->hour % 12 ? dt->hour % 12 : 12;

    if (sb_24h) {
        snprintf(c->status, sizeof(c->status), "%02d:%02d", dt->hour, dt->minute);
        snprintf(c->big, sizeof(c->big), "%02d:%02d", dt->hour, dt->minute);
    } else {
        snprintf(c->status, sizeof(c->status), "%d:%02d %s", h12, dt->minute, dt->hour < 12 ? "AM" : "PM");
        snprintf(c->big, sizeof(c->big), "%d:%02d", h12, dt->minute);
    }
    snprintf(c->date, sizeof(c->date), "%s, %s %d", weekdays[dt->weekday % 7],
             months[(dt->month + 11) % 12], dt->day);
    strncpy(c->weekday, weekdays[dt->weekday % 7], sizeof(c->weekday) - 1);
    c->day = dt->day;
}

/* ---- status bar --------------------------------------------------------- */

static void draw_battery(gfx_surface *s, int x, int y, uint32_t fg)
{
    gfx_fill(s, gfx_r(x, y, 20, 1), fg);
    gfx_fill(s, gfx_r(x, y + 8, 20, 1), fg);
    gfx_fill(s, gfx_r(x, y, 1, 9), fg);
    gfx_fill(s, gfx_r(x + 19, y, 1, 9), fg);
    gfx_fill(s, gfx_r(x + 20, y + 3, 2, 3), fg);
    gfx_vgradient(s, gfx_r(x + 2, y + 2, 16, 5), 0x9FE07A, 0x3E9E1E, 255);
}

static void draw_padlock(gfx_surface *s, int cx, int y, uint32_t fg)
{
    gfx_round_rect(s, gfx_r(cx - 4, y + 4, 9, 7), 1, fg, 255);
    gfx_fill(s, gfx_r(cx - 3, y, 1, 4), fg);
    gfx_fill(s, gfx_r(cx + 3, y, 1, 4), fg);
    gfx_fill(s, gfx_r(cx - 2, y - 1, 5, 1), fg);
}

void sb_draw_status(gfx_surface *s, const struct sb_clock *c, int style, bool padlock)
{
    gfx_rect r = gfx_r(0, 0, SCREEN_W, SB_STATUS_H);
    uint32_t fg;
    const gfx_font *f = ui_font(FONT_B10);

    if (style == SB_STATUS_GRAY) {
        gfx_vgradient(s, r, 0xE1E5EA, 0xA4ACB7, 255);
        gfx_fill(s, gfx_r(0, SB_STATUS_H - 1, SCREEN_W, 1), 0x5B6470);
        fg = 0x111111;
    } else {
        gfx_fill_alpha(s, r, 0x000000, style == SB_STATUS_BLACK ? 255 : 150);
        gfx_vgradient(s, gfx_r(0, 0, SCREEN_W, SB_STATUS_H / 2), 0xFFFFFF, 0xFFFFFF, 18);
        fg = 0xFFFFFF;
    }
    /* No radios on the BE-300: show the device name, like iPod touch did. */
    gfx_text(s, 5, 2, f, "BE-300", fg, 255);
    if (padlock)
        draw_padlock(s, SCREEN_W / 2, 3, fg);
    else
        gfx_text_center(s, 0, 2, SCREEN_W, f, c->status, fg, 255);
    draw_battery(s, SCREEN_W - 26, 3, fg);
}

/* ---- shared wallpaper --------------------------------------------------- */

void sb_draw_wallpaper(gfx_surface *s, const struct sb_view *v, gfx_rect r)
{
    int w = (v->wallpaper >= 0 && v->wallpaper < sb_num_wallpapers) ? v->wallpaper : 0;
    gfx_rect saved = s->clip, c;

    if (!gfx_rect_intersect(r, saved, &c))
        return;
    s->clip = c;
    if (walls_ok[w])
        gfx_draw_image(s, 0, 0, &walls[w], 255);
    else
        gfx_vgradient(s, r, 0x041733, 0x2A86C9, 255);
    s->clip = saved;
}

/* ---- lock screen -------------------------------------------------------- */

static void glass_panel(gfx_surface *s, gfx_rect r)
{
    gfx_fill_alpha(s, r, 0x000000, 150);
    gfx_vgradient(s, gfx_r(r.x, r.y, r.w, r.h / 2), 0xFFFFFF, 0xFFFFFF, 22);
    gfx_fill_alpha(s, gfx_r(r.x, r.y, r.w, 1), 0xFFFFFF, 50);
}

void sb_draw_lock_track(gfx_surface *s, const struct sb_view *v)
{
    gfx_rect track = gfx_r(TRACK_X, TRACK_Y, TRACK_W, TRACK_H);
    const gfx_font *f = ui_font(FONT_R17);
    const char *label = "slide to unlock";

    /* Recessed track: dark fill, lighter lower edge. */
    gfx_round_rect(s, gfx_r(track.x, track.y + 1, track.w, track.h), 9, 0x5A5A5A, 120);
    gfx_round_rect(s, track, 9, 0x000000, 200);
    gfx_round_rect_vgradient(s, gfx_r(track.x + 1, track.y + 1, track.w - 2, track.h / 2), 8, 0x101010, 0x202020, 160);

    /* Label fades out as the knob moves right; a soft highlight sweeps
     * across it while the knob is at rest. */
    int fade = 255 - v->knob_x * 255 * 2 / KNOB_TRAVEL;
    if (fade > 0) {
        int tw = gfx_text_width(f, label);
        int tx = track.x + KNOB_W + 4 + (track.w - KNOB_W - 4 - tw) / 2;
        int ty = track.y + (track.h - f->line_height) / 2;
        gfx_text(s, tx, ty, f, label, 0x8C8C8C, fade);
        if (v->knob_x == 0) {
            gfx_rect saved = s->clip, band;
            static const int widths[3] = { 30, 18, 8 };
            for (int i = 0; i < 3; i++) {
                if (gfx_rect_intersect(gfx_r(v->shimmer_x - widths[i], track.y, widths[i] * 2, track.h), saved, &band)) {
                    s->clip = band;
                    gfx_text(s, tx, ty, f, label, 0xFFFFFF, 110);
                }
            }
            s->clip = saved;
        }
    }
    gfx_draw_image(s, track.x + 2 + v->knob_x, track.y + 2, &knob, 255);
}

/* Copies str into out, cut with an ellipsis to fit in w pixels. */
static const char *fit_text(const gfx_font *f, const char *str, int w, char *out, int n)
{
    int len = (int)strlen(str);

    if (gfx_text_width(f, str) <= w)
        return str;
    int ell = gfx_text_width(f, GFX_CH_ELLIPSIS);
    while (len > 0 && (len + 2 > n || gfx_text_width_n(f, str, len) + ell > w))
        len--;
    while (len > 0 && str[len - 1] == ' ')
        len--;
    memcpy(out, str, (size_t)len);
    strcpy(out + len, GFX_CH_ELLIPSIS);
    return out;
}

/* Nearest-neighbour scaled image with its alpha, for small icons. */
static void draw_image_scaled(gfx_surface *s, int x, int y, int size, const gfx_image *img)
{
    gfx_rect c;

    if (!img->px || !gfx_rect_intersect(gfx_r(x, y, size, size), s->clip, &c))
        return;
    for (int dy = c.y; dy < c.y + c.h; dy++) {
        int sy = (dy - y) * img->h / size;
        uint16_t *row = s->px + dy * s->stride;
        for (int dx = c.x; dx < c.x + c.w; dx++) {
            int i = sy * img->w + (dx - x) * img->w / size;
            int a = img->alpha ? img->alpha[i] : 255;
            if (a)
                row[dx] = a == 255 ? img->px[i] : gfx_blend565(row[dx], img->px[i], a);
        }
    }
}

static void draw_note_text(gfx_surface *s, int x, int y, int w, const struct sb_note *n, const gfx_font *tf,
                           const gfx_font *bf, int gap, uint32_t body_rgb)
{
    char buf[sizeof(n->body) + 4];

    gfx_text(s, x, y, tf, fit_text(tf, n->title[0] ? n->title : sb_apps[n->app].label, w, buf, sizeof(buf)),
             0xFFFFFF, 255);
    if (n->body[0])
        gfx_text(s, x, y + gap, bf, fit_text(bf, n->body, w, buf, sizeof(buf)), body_rgb, 255);
}

/* Notes delivered while locked, newest first, between the two panels. */
static void draw_lock_notes(gfx_surface *s, const struct sb_view *v)
{
    for (int i = 0; i < v->num_lock_notes && i < SB_LOCK_NOTES; i++) {
        const struct sb_note *n = &v->lock_notes[i];
        gfx_rect r = gfx_r(0, SB_STATUS_H + LOCK_TOP_H + 2 + i * (LOCK_NOTE_H + 1), SCREEN_W, LOCK_NOTE_H);
        glass_panel(s, r);
        draw_image_scaled(s, r.x + 6, r.y + 4, 24, &icons[n->app]);
        draw_note_text(s, r.x + 36, r.y + 2, r.w - 42, n, ui_font(FONT_B11), ui_font(FONT_R11), 15, 0xD0D0D0);
    }
}

void sb_draw_banner(gfx_surface *s, const struct sb_note *n)
{
    gfx_rect r = gfx_r(0, SB_STATUS_H, SCREEN_W, BANNER_H);

    gfx_fill_alpha(s, r, 0x000000, 215);
    gfx_vgradient(s, gfx_r(r.x, r.y, r.w, r.h / 2), 0xFFFFFF, 0xFFFFFF, 26);
    gfx_fill(s, gfx_r(r.x, r.y + r.h - 1, r.w, 1), 0x000000);
    draw_image_scaled(s, r.x + 6, r.y + 6, 32, &icons[n->app]);
    draw_note_text(s, r.x + 46, r.y + 5, r.w - 52, n, ui_font(FONT_B12), ui_font(FONT_R12), 18, 0xD8D8D8);
}

void sb_draw_lock(gfx_surface *s, const struct sb_view *v, int open)
{
    gfx_rect saved = s->clip, part;

    if (open == 0 && v->num_lock_notes > 0)
        draw_lock_notes(s, v);

    /* Top: status bar plus the clock panel, sliding up by `open`. */
    if (gfx_rect_intersect(gfx_r(0, 0, SCREEN_W, SB_STATUS_H + LOCK_TOP_H - open), saved, &part)) {
        s->clip = part;
        sb_draw_wallpaper(s, v, gfx_r(0, 0, SCREEN_W, SB_STATUS_H + LOCK_TOP_H));
        gfx_surface sh = *s;
        sh.px -= open * s->stride;      /* draw shifted up */
        sh.clip.y += open;
        sb_draw_status(&sh, &v->clk, -1, true);
        glass_panel(&sh, gfx_r(0, SB_STATUS_H, SCREEN_W, LOCK_TOP_H));
        gfx_text_center(&sh, 0, SB_STATUS_H + 6, SCREEN_W, ui_font(FONT_R44), v->clk.big, 0xFFFFFF, 255);
        gfx_text_center(&sh, 0, SB_STATUS_H + 58, SCREEN_W, ui_font(FONT_R13), v->clk.date, 0xFFFFFF, 255);
    }
    /* Bottom: unlock slider panel, sliding down by `open`. */
    if (gfx_rect_intersect(gfx_r(0, LOCK_BOTTOM_Y + open, SCREEN_W, SCREEN_H - LOCK_BOTTOM_Y - open), saved, &part)) {
        s->clip = part;
        sb_draw_wallpaper(s, v, gfx_r(0, LOCK_BOTTOM_Y, SCREEN_W, SCREEN_H - LOCK_BOTTOM_Y));
        gfx_surface sh = *s;
        sh.px += open * s->stride;
        sh.clip.y -= open;
        glass_panel(&sh, gfx_r(0, LOCK_BOTTOM_Y, SCREEN_W, SCREEN_H - LOCK_BOTTOM_Y));
        sb_draw_lock_track(&sh, v);
    }
    s->clip = saved;
}

gfx_rect sb_passcode_area(void)
{
    return gfx_r(0, SB_STATUS_H, SCREEN_W, SCREEN_H - SB_STATUS_H);
}

void sb_draw_passcode(gfx_surface *s, const struct sb_view *v)
{
    sb_draw_wallpaper(s, v, gfx_r(0, 0, SCREEN_W, SCREEN_H));
    gfx_fill_alpha(s, gfx_r(0, 0, SCREEN_W, SCREEN_H), 0x000000, 150);
    sb_draw_status(s, &v->clk, -1, true);
    ui_draw_passcode(s, sb_passcode_area(), &v->pin);
}

/* ---- home screen -------------------------------------------------------- */

static int grid_index(int app)
{
    int n = 0;

    for (int i = 0; i < app; i++)
        n += sb_app_shown(i) && !sb_apps[i].dock;
    return n;
}

static int dock_index(int app, int *count)
{
    int n = 0, idx = 0;

    for (int i = 0; i < sb_num_apps; i++) {
        if (sb_app_shown(i) && sb_apps[i].dock) {
            if (i == app)
                idx = n;
            n++;
        }
    }
    *count = n;
    return idx;
}

int sb_num_pages(void)
{
    int n = 0;

    for (int i = 0; i < sb_num_apps; i++)
        n += sb_app_shown(i) && !sb_apps[i].dock;
    return n ? (n + PER_PAGE - 1) / PER_PAGE : 1;
}

gfx_rect sb_icon_rect(int app, int scroll_x)
{
    if (sb_apps[app].dock) {
        int count, idx = dock_index(app, &count);
        int span = count * CELL_W;
        int x0 = (SCREEN_W - span) / 2;
        return gfx_r(x0 + idx * CELL_W + (CELL_W - ICON_SIZE) / 2, DOCK_Y + 6, ICON_SIZE, ICON_SIZE);
    }
    int g = grid_index(app), page = g / PER_PAGE, cell = g % PER_PAGE;
    int x = page * SCREEN_W - scroll_x + (cell % GRID_COLS) * CELL_W + (CELL_W - ICON_SIZE) / 2;
    return gfx_r(x, GRID_TOP + (cell / GRID_COLS) * ROW_PITCH, ICON_SIZE, ICON_SIZE);
}

int sb_hit_icon(int x, int y, int scroll_x)
{
    for (int i = 0; i < sb_num_apps; i++) {
        if (!sb_app_shown(i))
            continue;
        gfx_rect r = sb_icon_rect(i, scroll_x);
        r = gfx_r(r.x - 6, r.y - 4, r.w + 12, r.h + 18);
        if (gfx_rect_contains(r, x, y))
            return i;
    }
    return -1;
}

gfx_rect sb_delete_rect(int app, int scroll_x)
{
    gfx_rect r = sb_icon_rect(app, scroll_x);

    return gfx_r(r.x - 12, r.y - 12, 26, 26);
}

/* Black circled X at the icon's top-left corner, for deleting it. */
static void draw_delete_button(gfx_surface *s, gfx_rect icon)
{
    int cx = icon.x + 1, cy = icon.y + 1;

    gfx_circle(s, cx, cy + 1, 10, 0x000000, 70);                /* shadow */
    gfx_circle(s, cx, cy, 10, 0xFFFFFF, 255);
    gfx_circle(s, cx, cy, 8, 0x202020, 255);
    for (int d = -3; d <= 3; d++) {
        gfx_fill(s, gfx_r(cx + d, cy + d, 2, 2), 0xFFFFFF);
        gfx_fill(s, gfx_r(cx + d, cy - d, 2, 2), 0xFFFFFF);
    }
}

/* An installed app without an icon: a grey tile with its initial. */
static void draw_generic_icon(gfx_surface *s, gfx_rect r, const char *label)
{
    char initial[2] = { label[0], 0 };

    gfx_round_rect_vgradient(s, r, 9, 0x9AA3AE, 0x4E5763, 255);
    gfx_text_center(s, r.x, r.y + (r.h - ui_font(FONT_B20)->line_height) / 2, r.w, ui_font(FONT_B20),
                    initial, 0xFFFFFF, 255);
}

/* Red count at the icon's top-right corner, like iPhone OS. */
static void draw_badge(gfx_surface *s, gfx_rect icon, unsigned count)
{
    const gfx_font *f = ui_font(FONT_B10);
    char buf[8];

    snprintf(buf, sizeof(buf), count > 99 ? "99+" : "%u", count);
    int h = 18, w = gfx_text_width(f, buf) + 10;
    if (w < h)
        w = h;
    gfx_rect b = gfx_r(icon.x + icon.w - w + 6, icon.y - 5, w, h);
    gfx_round_rect(s, gfx_r(b.x, b.y + 1, b.w, b.h), h / 2, 0x000000, 70);    /* shadow */
    gfx_round_rect(s, b, h / 2, 0xFFFFFF, 255);
    gfx_round_rect_vgradient(s, gfx_r(b.x + 2, b.y + 2, b.w - 4, b.h - 4), h / 2 - 2, 0xF2443A, 0xC0140C, 255);
    gfx_text_center(s, b.x, b.y + (h - f->line_height) / 2, b.w, f, buf, 0xFFFFFF, 255);
}

static void draw_icon(gfx_surface *s, const struct sb_view *v, int app, gfx_rect r)
{
    const gfx_font *lab = ui_font(FONT_B9);

    if (v->jiggle)                      /* neighbours wobble in opposition */
        r.x += ((app + v->jiggle_phase) & 1) ? 1 : -1, r.y += ((app + v->jiggle_phase) & 2) ? 1 : 0;
    if (icon_ok[app])
        gfx_draw_image(s, r.x, r.y, &icons[app], 255);
    else
        draw_generic_icon(s, r, sb_apps[app].label);
    if (!strcmp(sb_apps[app].icon, "icon_calendar.gzi")) {
        /* Calendar's icon shows today's date. */
        char day[4];
        snprintf(day, sizeof(day), "%d", v->clk.day);
        gfx_text_center(s, r.x, r.y + 1, r.w, ui_font(FONT_B8), v->clk.weekday, 0xFFFFFF, 255);
        gfx_text_center(s, r.x, r.y + 14, r.w, ui_font(FONT_B20), day, 0x1A1A1A, 255);
    }
    if (v->pressed == app)
        gfx_round_rect(s, r, 8, 0x000000, 110);
    if (v->badges && v->badges[app])
        draw_badge(s, r, v->badges[app]);
    int tw = gfx_text_width(lab, sb_apps[app].label);
    gfx_text_shadow(s, r.x + (r.w - tw) / 2, r.y + r.h + 2, lab, sb_apps[app].label, 0xFFFFFF, 0x000000, 1, 255);
    if (v->jiggle && sb_apps[app].installed)
        draw_delete_button(s, r);
}

static void draw_dock(gfx_surface *s, const struct sb_view *v)
{
    gfx_rect d = gfx_r(0, DOCK_Y, SCREEN_W, SCREEN_H - DOCK_Y);

    /* Glass shelf: bright reflective top edge fading into a darker base. */
    gfx_vgradient(s, gfx_r(0, DOCK_Y, SCREEN_W, 30), 0xFFFFFF, 0xFFFFFF, 70);
    gfx_fill_alpha(s, gfx_r(0, DOCK_Y + 30, SCREEN_W, d.h - 30), 0xFFFFFF, 35);
    gfx_fill_alpha(s, gfx_r(0, DOCK_Y, SCREEN_W, 1), 0xFFFFFF, 160);
    gfx_fill_alpha(s, gfx_r(0, DOCK_Y + 1, SCREEN_W, 1), 0x000000, 60);
    for (int i = 0; i < sb_num_apps; i++) {
        if (sb_app_shown(i) && sb_apps[i].dock)
            draw_icon(s, v, i, sb_icon_rect(i, 0));
    }
}

static void draw_page_dots(gfx_surface *s, const struct sb_view *v)
{
    int pages = sb_num_pages();

    if (pages < 2)
        return;
    int cur = (v->scroll_x + SCREEN_W / 2) / SCREEN_W;
    int x0 = SCREEN_W / 2 - (pages * 10) / 2 + 5;
    for (int p = 0; p < pages; p++)
        gfx_circle(s, x0 + p * 10, PAGE_DOTS_Y, 3, 0xFFFFFF, p == cur ? 255 : 90);
}

void sb_draw_home(gfx_surface *s, const struct sb_view *v)
{
    gfx_rect saved = s->clip, grid;

    sb_draw_wallpaper(s, v, gfx_r(0, 0, SCREEN_W, SCREEN_H));
    sb_draw_status(s, &v->clk, -1, false);
    if (gfx_rect_intersect(gfx_r(0, SB_STATUS_H, SCREEN_W, DOCK_Y - SB_STATUS_H), saved, &grid)) {
        s->clip = grid;
        for (int i = 0; i < sb_num_apps; i++) {
            if (sb_app_shown(i) && !sb_apps[i].dock) {
                gfx_rect r = sb_icon_rect(i, v->scroll_x);
                if (r.x + r.w + 20 > 0 && r.x - 20 < SCREEN_W)
                    draw_icon(s, v, i, r);
            }
        }
        draw_page_dots(s, v);
        s->clip = saved;
    }
    draw_dock(s, v);
}
