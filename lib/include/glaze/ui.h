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
    bool hardware_keyboard;
    struct sb_shared *shared;
    gfx_surface surf;
    gfx_rect dirty;
};

extern struct ui_app ui;

/* Connects to SpringBoard and maps the app surface; exits on failure. */
void ui_init(int status_style);
bool ui_in_background(void);
/* After Home, ui_next_event returns GZ_EV_APP_DEACTIVATE, the moment to
 * save anything unsaved and release shared resources (an audio stream):
 * the next call suspends the app until GZ_EV_APP_ACTIVATE, and a suspended
 * app may be ended without warning when memory or slots run short.
 * SpringBoard freezes it anyway a second later. An app with work to do in
 * the background (a network connection) turns suspension off here. */
void ui_set_background_running(bool on);
bool ui_has_hardware_keyboard(void);
/* Height reserved for the on-screen keyboard (zero with a dock). */
int ui_keyboard_height(void);
/* Physical text/Enter/Backspace, or zero; other key events stay available. */
int ui_input_char(const struct gz_input_event *e);
/* Edit an 8-bit text buffer at *cursor; handles movement/deletion/text. */
bool ui_edit_input(char *text, int cap, int *cursor, const struct gz_input_event *e, bool multiline);
/* Shared, volatile text clipboard (UI thread, foreground app only). set
 * returns 0; get returns the byte length, or an error without truncation. */
#define UI_CLIPBOARD_MAX SB_CLIPBOARD_MAX
int ui_clipboard_set(const char *text, int length);
int ui_clipboard_get(char *text, int capacity);
int ui_clipboard_clear(void);

enum { UI_EDIT_MULTILINE = 1, UI_EDIT_SECRET = 2, UI_EDIT_DIGITS = 4,
       UI_EDIT_URL = 8, UI_EDIT_FILENAME = 16, UI_EDIT_READONLY = 32 };
/* One active selection per process. Reset after programmatically replacing
 * a buffer. Returning to another field or moving its cursor resets selection.
 * A handled edit returns true, even if only the selection/clipboard changed. */
void ui_edit_reset(void);
bool ui_edit_event(char *text, int cap, int *cursor, const struct gz_input_event *e, unsigned flags);
bool ui_edit_key(char *text, int cap, int *cursor, int key, unsigned flags);
bool ui_edit_modified(void);
bool ui_edit_selecting(void);
const char *ui_edit_notice(void);
bool ui_edit_selection(const char *text, int cursor, int *start, int *end);
void ui_edit_set_cursor(char *text, int *cursor, int position, bool extend);
/* Highlight the selected part of one displayed byte range, before drawing
 * its text. Caller supplies clipping and uses the full buffer as `text`. */
void ui_draw_edit_selection(gfx_surface *s, const gfx_font *font, const char *text,
                            int cursor, int start, int length, int x, int y, int height);
/* Next input event, waiting up to timeout_ms (-1 = forever). 1 = event,
 * 0 = timeout. */
int ui_next_event(struct gz_input_event *ev, int timeout_ms);
void ui_invalidate(gfx_rect r);
void ui_invalidate_all(void);
/* Push the dirty region to the screen (blocks until it is shown). */
void ui_present(void);

/* ---- badge and notifications (kept by SpringBoard, see glaze/sb.h) ------ */

/* The app's home-screen icon badge; 0 clears it. */
int ui_set_badge(int count);
/* Schedules a notification for wall-clock time `when` (ui_now_secs() scale;
 * 0 = now) that adds `badge` to the icon badge as it fires. Posting an id
 * again replaces it. If the app is in front then, it gets GZ_EV_APP_NOTIFY
 * instead of a banner and no badge. 0, or GZ_ENOSPC when SpringBoard's
 * table is full. */
int ui_notify_post(uint32_t id, uint32_t when, const char *title, const char *body, int badge);
int ui_notify_cancel(uint32_t id);
/* Fire time of a pending notification, or 0. */
uint32_t ui_notify_pending(uint32_t id);
/* Current wall-clock time in glaze/notify.h seconds. */
uint32_t ui_now_secs(void);

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
 * draws a back button whose rect is stored in *back_rect. Style 0 = blue,
 * 1 = black, 2 = tan leather (Notes). */
gfx_rect ui_draw_navbar(gfx_surface *s, const char *title, const char *back, gfx_rect *back_rect, int style);
/* A bordered bar button on the right of the nav bar ("Edit", "+", "Done"). */
gfx_rect ui_draw_bar_button(gfx_surface *s, const char *label, bool right, bool pressed, bool blue, int style);
void ui_draw_pinstripes(gfx_surface *s, gfx_rect r);
void ui_draw_button(gfx_surface *s, gfx_rect r, const char *label, bool pressed);
void ui_draw_switch(gfx_surface *s, int x, int y, bool on);
/* A horizontal slider whose thumb travels across `r` (also its touch area),
 * at `value` of 0..max; ui_slider_value maps a touch's x to a value. */
#define UI_SLIDER_THUMB 20
void ui_draw_slider(gfx_surface *s, gfx_rect r, int value, int max);
int ui_slider_value(gfx_rect r, int max, int x);
#define UI_SWITCH_W 64
#define UI_SWITCH_H 22
/* Grouped-table cell background; position: 0 only, 1 top, 2 middle, 3 bottom. */
void ui_draw_group_cell(gfx_surface *s, gfx_rect r, int position, bool selected);
/* ui_draw_group_cell's position for row i of n. */
int ui_group_pos(int i, int n);
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

/* ---- lists ---------------------------------------------------------------- */

/* A scrolling column of equal-height rows. Content is pad + count * row_h +
 * footer_h; rows start pad px below the top of area. */
struct ui_list {
    struct ui_scroll sc;
    gfx_rect area;          /* visible body, screen coordinates */
    int row_h, pad, footer_h;
    int count;
    int pressed;            /* row under the finger, or -1 */
};

#define UI_LIST_SEPARATOR   0xD9D9D9

typedef void (*ui_list_row_fn)(gfx_surface *s, int i, gfx_rect r, bool selected, void *ctx);

void ui_list_init(struct ui_list *l, gfx_rect area, int row_h);
/* Set the row count and scroll back to the top. */
void ui_list_reset(struct ui_list *l, int count);
/* Change the row count, keeping the scroll position where it still fits. */
void ui_list_set_count(struct ui_list *l, int count);
/* Resize the list's area, keeping the scroll position where it still fits. */
void ui_list_set_area(struct ui_list *l, gfx_rect area);
int ui_list_row_y(const struct ui_list *l, int i);
/* Row at screen y, or -1. */
int ui_list_row_at(const struct ui_list *l, int y);
/* Feed a touch event. Returns the row tapped (on touch up, without
 * scrolling, still over the row pressed), or -1. Sets *dirty when the list
 * needs a redraw; touches that begin outside area are ignored. */
int ui_list_event(struct ui_list *l, const struct gz_input_event *e, bool *dirty);
/* Call draw_row for each visible row, clipped to area. */
void ui_list_draw(gfx_surface *s, const struct ui_list *l, ui_list_row_fn draw_row, void *ctx);
/* Plain-list row background: the selection gradient and a bottom separator. */
void ui_draw_list_row(gfx_surface *s, gfx_rect r, bool selected);

/* Edit-mode deletion: tapping a row's badge arms it (the bar turns upright)
 * and shows a Delete button; tapping Delete confirms, and any other tap,
 * leaving edit mode or Back cancels. */
#define UI_DELETE_BADGE_W   26  /* badge column, from the row's left margin */
void ui_draw_delete_badge(gfx_surface *s, int cx, int cy, bool armed);
/* Draws the button with its right edge at right, centred on cy; returns its rect. */
gfx_rect ui_draw_delete_button(gfx_surface *s, int right, int cy, bool pressed);

/* ---- tab bar ------------------------------------------------------------ */

struct ui_tab {
    const char *label;
    const char *icon;       /* res mask image, 24x24 */
};
void ui_draw_tabbar(gfx_surface *s, const struct ui_tab *tabs, int n, int selected);
int ui_tabbar_hit(int n, int x, int y);

/* ---- keyboard ----------------------------------------------------------- */

#define UI_KB_H     150
#define UI_INPUT_PREF "ui.input_method"
enum { UI_INPUT_QWERTY, UI_INPUT_HANDWRITING };

struct ui_keyboard {
    bool shift, numbers;
    bool url;               /* web address layout: / . .com Go */
    uint8_t method;          /* occupies the original padding byte; size remains 8 */
    int pressed;            /* key index, or -1 */
};

/* ui_keyboard_event's return for the ".com" key of the URL layout. */
#define UI_KEY_DOTCOM   0x100
#define UI_KEY_SELECT_ALL 0x101
#define UI_KEY_COPY       0x102
#define UI_KEY_CUT        0x103
#define UI_KEY_PASTE      0x104
#define UI_KEY_SELECT     0x105
#define UI_KEY_LEFT       0x106
#define UI_KEY_RIGHT      0x107
#define UI_KEY_CLEAR_CLIP 0x108

void ui_keyboard_init(struct ui_keyboard *kb);
/* Opt in to the Edit panel; legacy keyboards never return edit commands.
 * Route every returned key through ui_edit_key/event. Flags protect secret
 * fields and enforce numeric/URL/filename input rules. Structure ABI unchanged. */
void ui_keyboard_edit_init(struct ui_keyboard *kb, unsigned flags);
/* Default for newly initialized panels; changing a panel does not save it. */
int ui_input_method_default(void);
int ui_input_method_save(int method);
void ui_keyboard_set_method(struct ui_keyboard *kb,int method);
void ui_draw_keyboard(gfx_surface *s, const struct ui_keyboard *kb, int y);
/* Returns a character ('\b' backspace, '\n' return, UI_KEY_DOTCOM) when a
 * key is released, 0 otherwise; redraw if *changed is set. */
int ui_keyboard_event(struct ui_keyboard *kb, const struct gz_input_event *e, int y, bool *changed);

/* ---- alert -------------------------------------------------------------- */

/* Draws a modal alert over the whole surface; returns button rects. The
 * message may have two lines, split by a newline. pressed: 0 or 1, or -1. */
void ui_draw_alert(gfx_surface *s, const char *title, const char *msg, const char *b1, const char *b2,
                   gfx_rect *r1, gfx_rect *r2, int pressed);

/* ---- passcode keypad ---------------------------------------------------- */

#define UI_PASSCODE_LEN     4
#define UI_PASSCODE_CANCEL  0x1B    /* ui_passcode_hit's return for `cancel` */

struct ui_passcode {
    const char *title, *msg;
    const char *cancel;     /* label of the bottom-left key, or NULL */
    int digits;             /* boxes filled */
    int pressed;            /* key under the finger (a hit value), or -1 */
    bool dark;              /* dark text, for a light background */
};

/* Title, digit boxes and message at the top of `area`, keypad at its bottom. */
void ui_draw_passcode(gfx_surface *s, gfx_rect area, const struct ui_passcode *pc);
/* '0'..'9', '\b', UI_PASSCODE_CANCEL, or -1. */
int ui_passcode_hit(gfx_rect area, const struct ui_passcode *pc, int x, int y);

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
