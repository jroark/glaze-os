/* libui widgets in the iPhone OS 1-3 style. */

#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdlib.h>
#include <string.h>

static gfx_image back_img[2];
static bool back_loaded[2];

static const gfx_image *back_image(int style)
{
    style = style ? 1 : 0;
    if (!back_loaded[style])
        back_loaded[style] = res_image(&back_img[style], style ? "ui_back_dark.gzi" : "ui_back.gzi");
    return &back_img[style];
}

gfx_rect ui_draw_navbar(gfx_surface *s, const char *title, const char *back, gfx_rect *back_rect, int style)
{
    gfx_rect r = gfx_r(0, 0, UI_W, UI_NAV_H);
    int half = UI_NAV_H / 2;

    if (style) {
        gfx_vgradient(s, gfx_r(0, 0, UI_W, half), 0x6A6A6A, 0x3C3C3C, 255);
        gfx_vgradient(s, gfx_r(0, half, UI_W, UI_NAV_H - half), 0x2A2A2A, 0x111111, 255);
    } else {
        gfx_vgradient(s, gfx_r(0, 0, UI_W, half), 0xB4C0D0, 0x8B9DB5, 255);
        gfx_vgradient(s, gfx_r(0, half, UI_W, UI_NAV_H - half), 0x8093AD, 0x6D84A2, 255);
    }
    gfx_fill(s, gfx_r(0, UI_NAV_H - 1, UI_W, 1), style ? 0x000000 : UI_NAV_LINE);
    gfx_fill_alpha(s, gfx_r(0, 0, UI_W, 1), 0xFFFFFF, 90);

    const gfx_font *tf = ui_font(FONT_B14);
    int left_edge = 6;
    if (back) {
        const gfx_font *bf = ui_font(FONT_B10);
        int bw = gfx_text_width(bf, back) + 22;
        gfx_rect br = gfx_r(5, (UI_NAV_H - 24) / 2, bw, 24);
        gfx_draw_3slice(s, br, back_image(style), 12, 6, 255);
        gfx_text_shadow(s, br.x + 14, br.y + (24 - bf->line_height) / 2, bf, back, 0xFFFFFF, 0x2A3442, -1, 255);
        if (back_rect)
            *back_rect = gfx_r(0, 0, br.x + br.w + 6, UI_NAV_H);
        left_edge = br.x + br.w + 6;
    }
    if (title) {
        int tw = gfx_text_width(tf, title);
        int tx = (UI_W - tw) / 2;
        if (tx < left_edge)
            tx = left_edge;
        gfx_text_shadow(s, tx, (UI_NAV_H - tf->line_height) / 2, tf, title, 0xFFFFFF,
                        style ? 0x000000 : 0x3E4E66, -1, 255);
    }
    return r;
}

gfx_rect ui_draw_bar_button(gfx_surface *s, const char *label, bool right, bool pressed, bool blue, int style)
{
    const gfx_font *f = ui_font(FONT_B10);
    int w = MAX(gfx_text_width(f, label) + 16, 30);
    gfx_rect r = gfx_r(right ? UI_W - w - 5 : 5, (UI_NAV_H - 24) / 2, w, 24);
    uint32_t top, bottom;

    if (blue) {
        top = 0x5E95F2; bottom = 0x2A63D4;
    } else if (style) {
        top = 0x5A5A5A; bottom = 0x262626;
    } else {
        top = 0x7E93AF; bottom = 0x4A6383;
    }
    if (pressed) {
        top = (top >> 1) & 0x7F7F7F;
        bottom = (bottom >> 1) & 0x7F7F7F;
    }
    gfx_round_rect(s, r, 5, 0x2A3442, 220);
    gfx_round_rect_vgradient(s, gfx_r(r.x + 1, r.y + 1, r.w - 2, r.h - 2), 4, top, bottom, 255);
    gfx_round_rect(s, gfx_r(r.x + 1, r.y + 1, r.w - 2, (r.h - 2) / 2), 4, 0xFFFFFF, 30);
    gfx_text_shadow(s, r.x + (r.w - gfx_text_width(f, label)) / 2, r.y + (r.h - f->line_height) / 2, f, label,
                    0xFFFFFF, 0x2A3442, -1, 255);
    return r;
}

void ui_draw_pinstripes(gfx_surface *s, gfx_rect r)
{
    gfx_fill(s, r, UI_GROUP_BG);
    for (int x = r.x + 4; x < r.x + r.w; x += 6)
        gfx_fill(s, gfx_r(x, r.y, 2, r.h), UI_GROUP_STRIPE);
}

void ui_draw_button(gfx_surface *s, gfx_rect r, const char *label, bool pressed)
{
    const gfx_font *f = ui_font(FONT_B13);

    gfx_round_rect(s, r, 8, 0x8A8F96, 255);
    if (pressed)
        gfx_round_rect_vgradient(s, gfx_r(r.x + 1, r.y + 1, r.w - 2, r.h - 2), 7, UI_SELECT_TOP, UI_SELECT_BOTTOM, 255);
    else
        gfx_round_rect(s, gfx_r(r.x + 1, r.y + 1, r.w - 2, r.h - 2), 7, 0xFFFFFF, 255);
    gfx_text_center(s, r.x, r.y + (r.h - f->line_height) / 2, r.w, f, label, pressed ? 0xFFFFFF : UI_TEXT_DETAIL, 255);
}

void ui_draw_switch(gfx_surface *s, int x, int y, bool on)
{
    gfx_rect r = gfx_r(x, y, UI_SWITCH_W, UI_SWITCH_H);
    const gfx_font *f = ui_font(FONT_B11);
    int knob_w = 28;
    gfx_rect saved = s->clip, c;

    gfx_round_rect(s, r, 5, 0x7D7D7D, 255);
    gfx_rect inner = gfx_r(x + 1, y + 1, r.w - 2, r.h - 2);
    if (gfx_rect_intersect(inner, saved, &c)) {
        s->clip = c;
        if (on) {
            gfx_round_rect_vgradient(s, inner, 4, 0x1F5FD4, 0x5A9BF4, 255);
            gfx_text_center(s, x, y + (r.h - f->line_height) / 2, r.w - knob_w, f, "ON", 0xFFFFFF, 255);
        } else {
            gfx_round_rect_vgradient(s, inner, 4, 0xD5D5D5, 0xFDFDFD, 255);
            gfx_text_center(s, x + knob_w, y + (r.h - f->line_height) / 2, r.w - knob_w, f, "OFF", 0x7A7A7A, 255);
        }
        s->clip = saved;
    }
    gfx_rect k = gfx_r(on ? x + r.w - knob_w : x, y, knob_w, r.h);
    gfx_round_rect(s, k, 5, 0x8A8A8A, 255);
    gfx_round_rect_vgradient(s, gfx_r(k.x + 1, k.y + 1, k.w - 2, k.h - 2), 4, 0xFFFFFF, 0xD0D0D0, 255);
}

void ui_draw_group_cell(gfx_surface *s, gfx_rect r, int position, bool selected)
{
    const int rad = 8;
    gfx_rect saved = s->clip, c;

    if (!gfx_rect_intersect(r, saved, &c))
        return;
    s->clip = c;
    /* Extend the rounded rect past the edges that should stay square. */
    gfx_rect shape = r;
    if (position == 2 || position == 3) {
        shape.y -= rad;
        shape.h += rad;
    }
    if (position == 1 || position == 2)
        shape.h += rad;
    gfx_round_rect(s, shape, rad, UI_CELL_BORDER, 255);
    gfx_rect in = gfx_r(shape.x + 1, shape.y + 1, shape.w - 2, shape.h - 2);
    if (selected)
        gfx_round_rect_vgradient(s, in, rad - 1, UI_SELECT_TOP, UI_SELECT_BOTTOM, 255);
    else
        gfx_round_rect(s, in, rad - 1, 0xFFFFFF, 255);
    if (position == 1 || position == 2)
        gfx_fill(s, gfx_r(r.x + 1, r.y + r.h - 1, r.w - 2, 1), 0xD9D9D9);
    s->clip = saved;
}

void ui_draw_chevron(gfx_surface *s, int x, int y, uint32_t rgb)
{
    gfx_line_aa(s, x * 16, y * 16, (x + 4) * 16, (y + 5) * 16, 32, rgb, 255);
    gfx_line_aa(s, (x + 4) * 16, (y + 5) * 16, x * 16, (y + 10) * 16, 32, rgb, 255);
}

/* ---- tab bar ------------------------------------------------------------ */

static gfx_image tab_icons[8];
static const char *tab_icon_names[8];

void ui_draw_tabbar(gfx_surface *s, const struct ui_tab *tabs, int n, int selected)
{
    gfx_rect r = gfx_r(0, UI_H - UI_TAB_H, UI_W, UI_TAB_H);
    const gfx_font *f = ui_font(FONT_B8);
    int w = UI_W / n;

    gfx_vgradient(s, gfx_r(r.x, r.y, r.w, r.h / 2), 0x3A3A3A, 0x202020, 255);
    gfx_fill(s, gfx_r(r.x, r.y + r.h / 2, r.w, r.h - r.h / 2), 0x0C0C0C);
    gfx_fill(s, gfx_r(r.x, r.y, r.w, 1), 0x555555);
    for (int i = 0; i < n && i < 8; i++) {
        int x = i * w;
        if (tab_icon_names[i] != tabs[i].icon) {
            res_image(&tab_icons[i], tabs[i].icon);
            tab_icon_names[i] = tabs[i].icon;
        }
        if (i == selected)
            gfx_round_rect(s, gfx_r(x + 3, r.y + 2, w - 6, r.h - 4), 4, 0xFFFFFF, 28);
        int ix = x + (w - 24) / 2;
        if (i == selected) {
            gfx_draw_mask(s, ix, r.y + 2, &tab_icons[i], 0x7CC4FF, 255);
            gfx_rect saved = s->clip, half;
            if (gfx_rect_intersect(gfx_r(ix, r.y + 2 + 12, 24, 12), saved, &half)) {
                s->clip = half;
                gfx_draw_mask(s, ix, r.y + 2, &tab_icons[i], 0x1E7FE6, 255);
                s->clip = saved;
            }
        } else {
            gfx_draw_mask(s, ix, r.y + 2, &tab_icons[i], 0x8E8E8E, 255);
        }
        gfx_text_center(s, x, r.y + 24, w, f, tabs[i].label, i == selected ? 0xFFFFFF : 0x9A9A9A, 255);
    }
}

int ui_tabbar_hit(int n, int x, int y)
{
    if (y < UI_H - UI_TAB_H || x < 0 || x >= UI_W)
        return -1;
    return x / (UI_W / n);
}

/* ---- alert -------------------------------------------------------------- */

void ui_draw_alert(gfx_surface *s, const char *title, const char *msg, const char *b1, const char *b2,
                   gfx_rect *r1, gfx_rect *r2, int pressed)
{
    gfx_rect box = gfx_r(14, 90, UI_W - 28, 124);
    const gfx_font *tf = ui_font(FONT_B14), *mf = ui_font(FONT_R12), *bf = ui_font(FONT_B12);

    gfx_fill_alpha(s, gfx_r(0, 0, UI_W, UI_H), 0x000000, 110);
    gfx_round_rect(s, gfx_r(box.x - 2, box.y - 2, box.w + 4, box.h + 4), 10, 0xE4E8F0, 230);
    gfx_round_rect_vgradient(s, box, 9, 0x2F4A7D, 0x0E2550, 235);
    gfx_round_rect(s, gfx_r(box.x, box.y, box.w, 40), 9, 0xFFFFFF, 30);
    gfx_text_center(s, box.x, box.y + 10, box.w, tf, title, 0xFFFFFF, 255);
    if (msg)
        gfx_text_center(s, box.x, box.y + 34, box.w, mf, msg, 0xFFFFFF, 255);
    int bw = b2 ? (box.w - 30) / 2 : box.w - 20;
    gfx_rect a = gfx_r(box.x + 10, box.y + box.h - 42, bw, 30);
    gfx_rect b = gfx_r(a.x + bw + 10, a.y, bw, 30);
    for (int i = 0; i < (b2 ? 2 : 1); i++) {
        gfx_rect br = i ? b : a;
        bool p = pressed == i;
        gfx_round_rect(s, br, 6, 0x0A1A3A, 255);
        gfx_round_rect_vgradient(s, gfx_r(br.x + 1, br.y + 1, br.w - 2, br.h - 2), 5,
                                 p ? 0x5F7FB8 : 0x8EA2C8, p ? 0x2B4A86 : 0x4A6496, 255);
        gfx_text_center(s, br.x, br.y + (br.h - bf->line_height) / 2, br.w, bf, i ? b2 : b1, 0xFFFFFF, 255);
    }
    if (r1)
        *r1 = a;
    if (r2)
        *r2 = b;
}

/* ---- inertial scrolling -------------------------------------------------- */

int ui_scroll_max(const struct ui_scroll *sc)
{
    return sc->content_h > sc->view_h ? sc->content_h - sc->view_h : 0;
}

void ui_scroll_reset(struct ui_scroll *sc, int content_h, int view_h)
{
    memset(sc, 0, sizeof(*sc));
    sc->content_h = content_h;
    sc->view_h = view_h;
}

static int rubber(const struct ui_scroll *sc, int off)
{
    int max = ui_scroll_max(sc);

    if (off < 0)
        return off / 2;
    if (off > max)
        return max + (off - max) / 2;
    return off;
}

bool ui_scroll_event(struct ui_scroll *sc, const struct gz_input_event *e, int y)
{
    uint32_t t = gz_time_ms();
    int old = sc->offset;

    switch (e->type) {
    case GZ_EV_TOUCH_DOWN:
        sc->dragging = true;
        sc->moved = sc->animating;      /* a touch that stops a fling isn't a tap */
        sc->animating = false;
        sc->velocity = 0;
        sc->down_y = sc->last_y = y;
        sc->last_ms = t;
        sc->anim_from = sc->offset;     /* drag origin */
        break;
    case GZ_EV_TOUCH_MOVE:
        if (!sc->dragging)
            break;
        if (abs(y - sc->down_y) > 6)
            sc->moved = true;
        if (sc->moved) {
            uint32_t dt = t - sc->last_ms;
            if (dt > 0) {
                int v = (sc->last_y - y) * 1000 / (int)dt;
                sc->velocity = (sc->velocity + v * 2) / 3;
            }
            sc->offset = rubber(sc, sc->anim_from + (sc->down_y - y));
        }
        sc->last_y = y;
        sc->last_ms = t;
        break;
    case GZ_EV_TOUCH_UP:
        if (!sc->dragging)
            break;
        sc->dragging = false;
        if (t - sc->last_ms > 100)
            sc->velocity = 0;           /* finger rested before lifting */
        if (sc->moved) {
            sc->animating = true;
            sc->anim_t0 = t;
            sc->last_ms = t;
        }
        break;
    }
    return sc->offset != old;
}

bool ui_scroll_step(struct ui_scroll *sc)
{
    if (!sc->animating)
        return false;
    uint32_t t = gz_time_ms();
    int dt = (int)(t - sc->last_ms);
    int max = ui_scroll_max(sc);

    if (dt <= 0)
        return true;
    sc->last_ms = t;
    if (sc->offset < 0 || sc->offset > max) {
        /* Spring back toward the nearest edge. */
        int target = sc->offset < 0 ? 0 : max;
        int d = target - sc->offset;
        int stepd = d * MIN(dt, 100) / 100;
        if (stepd == 0)
            stepd = d > 0 ? 1 : -1;
        sc->offset += stepd;
        sc->velocity = 0;
        if (sc->offset == target)
            sc->animating = false;
        return true;
    }
    sc->offset += sc->velocity * dt / 1000;
    /* Exponential decay, roughly halving every 230 ms. */
    sc->velocity = sc->velocity * (1000 - 3 * MIN(dt, 300)) / 1000;
    if (sc->offset < -40)
        sc->offset = -40;
    if (sc->offset > max + 40)
        sc->offset = max + 40;
    if (abs(sc->velocity) < 20 && sc->offset >= 0 && sc->offset <= max)
        sc->animating = false;
    return true;
}
