/*
 * libui: the app side of SpringBoard plus early-iPhone-OS-style widgets.
 */
#ifndef GLAZE_UI_H
#define GLAZE_UI_H

#include <glaze/gfx.h>
#include <glaze/res.h>
#include <glaze/sb.h>

#define UI_W        SB_APP_W
#define UI_H        SB_APP_H

/* ---- app connection ----------------------------------------------------- */

struct ui_app {
    int sb_port, port;
    struct sb_shared *shared;
    gfx_surface surf;
    gfx_rect dirty;
};

extern struct ui_app ui;

/* Connects to SpringBoard and maps the app surface; exits on failure. */
void ui_init(int status_style);
/* Next input event, waiting up to timeout_ms (-1 = forever). 1 = event,
 * 0 = timeout. */
int ui_next_event(struct gz_input_event *ev, int timeout_ms);
void ui_invalidate(gfx_rect r);
void ui_invalidate_all(void);
/* Push the dirty region to the screen (blocks until it is shown). */
void ui_present(void);

/* ---- colours ------------------------------------------------------------ */

#define UI_NAV_TOP          0xB0BCCD
#define UI_NAV_BOTTOM       0x6D84A2
#define UI_NAV_LINE         0x2D3642
#define UI_GROUP_BG         0xC5CCD4    /* pinstripe base */
#define UI_GROUP_STRIPE     0xCBD2D8
#define UI_CELL_BORDER      0xA9ABAE
#define UI_TEXT             0x000000
#define UI_TEXT_DETAIL      0x385487    /* iPhone blue-gray value text */
#define UI_SELECT_TOP       0x058CF5
#define UI_SELECT_BOTTOM    0x015EE6
#define UI_BLUE_BUTTON      0x2A6CD9

/* ---- drawing helpers ----------------------------------------------------- */

#define UI_NAV_H            30
#define UI_ROW_H            30
#define UI_TAB_H            34

/* Navigation bar with centred title; returns its rect. `back` (may be NULL)
 * draws a back button whose rect is stored in *back_rect. Style 0 = blue
 * pinstripe, 1 = black. */
gfx_rect ui_draw_navbar(gfx_surface *s, const char *title, const char *back, gfx_rect *back_rect, int style);
/* A bordered bar button on the right of the nav bar ("Edit", "+", "Done"). */
gfx_rect ui_draw_bar_button(gfx_surface *s, const char *label, bool right, bool pressed, bool blue, int style);
void ui_draw_pinstripes(gfx_surface *s, gfx_rect r);
void ui_draw_button(gfx_surface *s, gfx_rect r, const char *label, bool pressed);
void ui_draw_switch(gfx_surface *s, int x, int y, bool on);
#define UI_SWITCH_W 64
#define UI_SWITCH_H 22
/* Grouped-table cell background; position: 0 only, 1 top, 2 middle, 3 bottom. */
void ui_draw_group_cell(gfx_surface *s, gfx_rect r, int position, bool selected);
void ui_draw_chevron(gfx_surface *s, int x, int y, uint32_t rgb);

/* ---- scroll views (inertial) -------------------------------------------- */

struct ui_scroll {
    int offset;             /* content offset, px */
    int content_h, view_h;
    bool dragging, moved;
    int last_y, down_y;
    uint32_t last_ms;
    int velocity;           /* px/s, positive = content moving up */
    int anim_from;
    uint32_t anim_t0;
    bool animating;
};

void ui_scroll_reset(struct ui_scroll *sc, int content_h, int view_h);
/* Feed a touch event (y relative to the view). Returns true if the offset
 * changed. A touch that ends without moving is a tap (sc->moved == false). */
bool ui_scroll_event(struct ui_scroll *sc, const struct gz_input_event *e, int y);
/* Advance inertial motion; returns true while animating. */
bool ui_scroll_step(struct ui_scroll *sc);
int ui_scroll_max(const struct ui_scroll *sc);

/* ---- tab bar ------------------------------------------------------------ */

struct ui_tab {
    const char *label;
    const char *icon;       /* res mask image, 24x24 */
};
void ui_draw_tabbar(gfx_surface *s, const struct ui_tab *tabs, int n, int selected);
int ui_tabbar_hit(int n, int x, int y);

/* ---- keyboard ----------------------------------------------------------- */

#define UI_KB_H     150

struct ui_keyboard {
    bool shift, numbers;
    int pressed;            /* key index, or -1 */
};

void ui_keyboard_init(struct ui_keyboard *kb);
void ui_draw_keyboard(gfx_surface *s, const struct ui_keyboard *kb, int y);
/* Returns a character ('\b' backspace, '\n' return) when a key is released,
 * 0 otherwise; redraw if *changed is set. */
int ui_keyboard_event(struct ui_keyboard *kb, const struct gz_input_event *e, int y, bool *changed);

/* ---- alert -------------------------------------------------------------- */

/* Draws a modal alert over the whole surface; returns button rects. */
void ui_draw_alert(gfx_surface *s, const char *title, const char *msg, const char *b1, const char *b2,
                   gfx_rect *r1, gfx_rect *r2, int pressed);

/* ---- navigation transition ------------------------------------------------ */

/* Slide from the current surface to the view drawn by draw_new (forward =
 * push, otherwise pop). Presents every frame; leaves the new view shown. */
void ui_transition(void (*draw_new)(gfx_surface *s), bool forward);

/* ---- text layout -------------------------------------------------------- */

/* Word-wrap `text` to `width`; fills line start offsets, returns the count. */
int ui_wrap(const gfx_font *f, const char *text, int width, int *starts, int max_lines);
/* Length of `line` excluding its newline. */
int ui_line_len(const char *text, const int *starts, int n, int line);
/* Character offset nearest x on a wrapped line. */
int ui_text_hit(const gfx_font *f, const char *text, const int *starts, int n, int line, int x);

/* ---- misc --------------------------------------------------------------- */

int ui_ease_out(uint32_t t, uint32_t dur);   /* 0..1024 */

#endif
