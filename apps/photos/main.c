/*
 * Photos: thumbnail grid and a full-screen viewer with swipe paging.
 * Tapping the viewer toggles the translucent bars, as in the original.
 */

#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NPHOTOS     6
#define THUMB       56
#define GAP         4
#define COLS        4
#define PAGE_MS     260

static gfx_image photos[NPHOTOS];
static gfx_surface thumbs[NPHOTOS];
static int nphotos;

enum view { V_GRID, V_VIEWER };
static enum view view = V_GRID;
static int current;
static int scroll_x;            /* viewer pan offset during swipes */
static bool bars = true;
static gfx_rect back_rect;
static int pressed_thumb = -1;

/* Viewer touch tracking. */
static bool touching, panning;
static int down_x, last_x, velocity;
static uint32_t last_ms;

static void make_thumbs(void)
{
    for (int i = 0; i < NPHOTOS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "photo_%d.gzi", i + 1);
        if (!res_image(&photos[i], name))
            break;
        /* Centre square crop scaled down once, then reused. */
        gfx_surface src, *t = &thumbs[i];
        gfx_surface_init(&src, (uint16_t *)photos[i].px, photos[i].w, photos[i].h, photos[i].w);
        gfx_surface_init(t, malloc(THUMB * THUMB * 2), THUMB, THUMB, THUMB);
        int side = MIN(photos[i].w, photos[i].h);
        gfx_blit_scaled(t, gfx_r(0, 0, THUMB, THUMB), &src,
                        gfx_r((photos[i].w - side) / 2, (photos[i].h - side) / 2, side, side), 255);
        nphotos = i + 1;
    }
}

static gfx_rect thumb_rect(int i)
{
    return gfx_r(GAP + (i % COLS) * (THUMB + GAP), UI_NAV_H + GAP + (i / COLS) * (THUMB + GAP), THUMB, THUMB);
}

static void draw_grid(gfx_surface *s)
{
    gfx_fill(s, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H), 0xFFFFFF);
    for (int i = 0; i < nphotos; i++) {
        gfx_rect r = thumb_rect(i);
        gfx_blit(s, r.x, r.y, &thumbs[i], gfx_r(0, 0, THUMB, THUMB));
        if (i == pressed_thumb)
            gfx_fill_alpha(s, r, 0x000000, 100);
    }
    char count[24];
    snprintf(count, sizeof(count), "%d Photos", nphotos);
    gfx_text_center(s, 0, thumb_rect(nphotos - 1).y + THUMB + 12, UI_W, ui_font(FONT_R14), count, 0x666666, 255);
    ui_draw_navbar(s, "Camera Roll", NULL, NULL, 1);
}

/* The photo fills the whole screen behind the status bar, so it is drawn
 * shifted up by the status bar height. */
static void draw_photo(gfx_surface *s, int i, int x)
{
    gfx_surface sh = *s;

    sh.px -= SB_STATUS_H * s->stride;
    sh.clip = gfx_r(s->clip.x, s->clip.y + SB_STATUS_H, s->clip.w, s->clip.h);
    gfx_draw_image(&sh, x, 0, &photos[i], 255);
}

static void draw_viewer(gfx_surface *s)
{
    gfx_fill(s, gfx_r(0, 0, UI_W, UI_H), 0x000000);
    draw_photo(s, current, -scroll_x);
    if (scroll_x > 0 && current + 1 < nphotos)
        draw_photo(s, current + 1, UI_W - scroll_x + 8);
    if (scroll_x < 0 && current > 0)
        draw_photo(s, current - 1, -UI_W - scroll_x - 8);
    if (bars) {
        char title[24];
        snprintf(title, sizeof(title), "%d of %d", current + 1, nphotos);
        /* Translucent bar: render it aside, then blend over the photo. */
        static uint16_t barbuf[UI_W * UI_NAV_H];
        gfx_surface bar;
        gfx_surface_init(&bar, barbuf, UI_W, UI_NAV_H, UI_W);
        ui_draw_navbar(&bar, title, "Camera Roll", &back_rect, 1);
        gfx_blit_alpha(s, 0, 0, &bar, gfx_r(0, 0, UI_W, UI_NAV_H), 200);
        gfx_rect tb = gfx_r(0, UI_H - 34, UI_W, 34);
        gfx_fill_alpha(s, tb, 0x000000, 140);
        gfx_vgradient(s, gfx_r(0, tb.y, UI_W, 17), 0xFFFFFF, 0xFFFFFF, 20);
        /* Previous / next arrows. */
        const gfx_font *f = ui_font(FONT_B20);
        gfx_text_center(s, 40, tb.y + 4, 40, f, GFX_CH_LARROW, current > 0 ? 0xFFFFFF : 0x666666, 255);
        gfx_text_center(s, UI_W - 80, tb.y + 4, 40, f, GFX_CH_RARROW, current + 1 < nphotos ? 0xFFFFFF : 0x666666, 255);
    }
}

static void draw_current(gfx_surface *s)
{
    if (view == V_GRID)
        draw_grid(s);
    else
        draw_viewer(s);
}

static void redraw(void)
{
    draw_current(&ui.surf);
    ui_invalidate_all();
    ui_present();
}

static void animate_to(int target_scroll, int next_index)
{
    int from = scroll_x;
    uint32_t t0 = gz_time_ms();

    for (;;) {
        int p = ui_ease_out(gz_time_ms() - t0, PAGE_MS);
        scroll_x = from + (target_scroll - from) * p / 1024;
        redraw();
        if (p >= 1024)
            break;
        gz_sleep_ms(10);
    }
    current = next_index;
    scroll_x = 0;
    redraw();
}

static void go_neighbour(int dir)
{
    int next = current + dir;

    if (next < 0 || next >= nphotos) {
        animate_to(0, current);
        return;
    }
    animate_to(dir * (UI_W + 8), next);
}

static void viewer_event(const struct gz_input_event *e)
{
    uint32_t t = gz_time_ms();

    switch (e->type) {
    case GZ_EV_TOUCH_DOWN:
        if (bars && gfx_rect_contains(back_rect, e->x, e->y) && e->y < UI_NAV_H) {
            view = V_GRID;
            ui_transition(draw_current, false);
            return;
        }
        if (bars && e->y >= UI_H - 34) {
            if (e->x < UI_W / 2)
                go_neighbour(-1);
            else
                go_neighbour(1);
            return;
        }
        touching = true;
        panning = false;
        down_x = last_x = e->x;
        last_ms = t;
        velocity = 0;
        break;
    case GZ_EV_TOUCH_MOVE:
        if (!touching)
            break;
        if (abs(e->x - down_x) > 8)
            panning = true;
        if (t > last_ms)
            velocity = (e->x - last_x) * 1000 / (int)(t - last_ms);
        last_x = e->x;
        last_ms = t;
        if (panning) {
            scroll_x = down_x - e->x;
            if ((scroll_x < 0 && current == 0) || (scroll_x > 0 && current + 1 == nphotos))
                scroll_x /= 3;          /* resist past the ends */
            redraw();
        }
        break;
    case GZ_EV_TOUCH_UP:
        if (!touching)
            break;
        touching = false;
        if (!panning) {
            bars = !bars;
            redraw();
        } else if (scroll_x > UI_W / 3 || velocity < -400) {
            go_neighbour(1);
        } else if (scroll_x < -UI_W / 3 || velocity > 400) {
            go_neighbour(-1);
        } else {
            animate_to(0, current);
        }
        break;
    }
}

static int thumb_hit(int x, int y)
{
    for (int i = 0; i < nphotos; i++) {
        if (gfx_rect_contains(thumb_rect(i), x, y))
            return i;
    }
    return -1;
}

static void grid_event(const struct gz_input_event *e)
{
    if (e->type == GZ_EV_TOUCH_DOWN) {
        pressed_thumb = thumb_hit(e->x, e->y);
        redraw();
    } else if (e->type == GZ_EV_TOUCH_UP) {
        int i = pressed_thumb;
        pressed_thumb = -1;
        if (i >= 0 && thumb_hit(e->x, e->y) == i) {
            current = i;
            scroll_x = 0;
            bars = true;
            view = V_VIEWER;
            ui_transition(draw_current, true);
        } else {
            redraw();
        }
    }
}

int main(void)
{
    ui_init(SB_STATUS_BLACK);
    make_thumbs();
    redraw();

    for (;;) {
        struct gz_input_event ev;
        if (!ui_next_event(&ev, -1))
            continue;
        if (view == V_GRID)
            grid_event(&ev);
        else if (ev.type == GZ_EV_KEY_DOWN && (ev.code == GZ_KEY_LEFT || ev.code == GZ_KEY_RIGHT))
            go_neighbour(ev.code == GZ_KEY_LEFT ? -1 : 1);
        else if (ev.type == GZ_EV_KEY_DOWN && ev.code == GZ_KEY_ESC) {
            view = V_GRID;
            ui_transition(draw_current, false);
        } else
            viewer_event(&ev);
    }
}
