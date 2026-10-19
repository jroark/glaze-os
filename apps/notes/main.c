/*
 * Notes: a list of notes on yellow legal paper and an editor with the
 * on-screen keyboard. Notes persist as files under /Library/Notes:
 *   index         array of struct meta (id + last edit time)
 *   <id>.txt      note text
 * Edits are saved after a short pause and on leaving the editor, since
 * pressing Home terminates the app.
 */

#include <glaze/fs.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <string.h>

#define MAX_NOTES   64
#define MAX_TEXT    8000
#define SAVE_IDLE   1500
#define MARGIN      12
#define LINE_H      22
#define TOOLBAR_H   34

#define PAPER       0xFCF4B4
#define RULE        0xD9CE8C
#define INK         0x2B1D0E
#define NAV_STYLE   2

struct meta {
    uint32_t id;
    struct gz_datetime dt;
};

static struct meta notes[MAX_NOTES];
static int nnotes;
static uint32_t next_id = 1;

enum view { V_LIST, V_EDIT };
static enum view view = V_LIST;

/* Editor state. */
static int cur = -1;                /* index into notes[] */
static char text[MAX_TEXT + 1];
static int cursor;
static bool editing;                /* keyboard visible */
static bool dirty;
static uint32_t last_edit;
static struct ui_keyboard kb;
static struct ui_scroll list_scroll, edit_scroll;
static int line_starts[600];
static int nlines;
static bool alert_open;
static int alert_pressed = -1;

/* Touch state. */
static int pressed_row = -1;
static bool pressed_plus, pressed_done, pressed_trash;
static gfx_rect back_rect, plus_rect, done_rect, trash_rect, alert_cancel, alert_delete;
static bool in_keyboard;

/* ---- storage ------------------------------------------------------------ */

#define NOTES_DIR   "/Library/Notes"
#define NOTES_INDEX NOTES_DIR "/index"

static void path_for(uint32_t id, char *buf, size_t n)
{
    snprintf(buf, n, NOTES_DIR "/%u.txt", id);
}

/* Writes a whole file, creating the Notes folder if it is missing. */
static void write_file(const char *path, const void *data, uint32_t len)
{
    if (fs_write_file(path, data, len) == GZ_ENOENT && fs_mkdirs(NOTES_DIR) == 0)
        fs_write_file(path, data, len);
}

static void load_index(void)
{
    int n = fs_read_file(NOTES_INDEX, notes, sizeof(notes));

    nnotes = n > 0 ? n / (int)sizeof(struct meta) : 0;
    for (int i = 0; i < nnotes; i++) {
        if (notes[i].id >= next_id)
            next_id = notes[i].id + 1;
    }
}

static void save_index(void)
{
    write_file(NOTES_INDEX, notes, (uint32_t)(nnotes * (int)sizeof(struct meta)));
}

static void load_text(int i, char *buf, int max)
{
    char path[FS_PATH_MAX];

    path_for(notes[i].id, path, sizeof(path));
    int n = fs_read_file(path, buf, (uint32_t)max);
    buf[n > 0 ? MIN(n, max) : 0] = 0;
}

/* Most recently edited note first, like the original. */
static void move_to_front(int i)
{
    struct meta m = notes[i];

    memmove(&notes[1], &notes[0], (size_t)i * sizeof(struct meta));
    notes[0] = m;
}

static void save_current(void)
{
    char path[FS_PATH_MAX];

    if (cur < 0 || !dirty)
        return;
    path_for(notes[cur].id, path, sizeof(path));
    write_file(path, text, (uint32_t)strlen(text));
    gz_rtc_get(&notes[cur].dt);
    move_to_front(cur);
    cur = 0;
    save_index();
    dirty = false;
}

static void delete_current(void)
{
    char path[FS_PATH_MAX];

    if (cur < 0)
        return;
    path_for(notes[cur].id, path, sizeof(path));
    fs_remove(path);
    memmove(&notes[cur], &notes[cur + 1], (size_t)(nnotes - cur - 1) * sizeof(struct meta));
    nnotes--;
    save_index();
    cur = -1;
    dirty = false;
}

/* ---- formatting --------------------------------------------------------- */

static const char *const month_abbr[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

static void short_time(const struct gz_datetime *dt, char *buf, size_t n)
{
    struct gz_datetime now;

    gz_rtc_get(&now);
    if (now.year == dt->year && now.month == dt->month && now.day == dt->day) {
        int h = dt->hour % 12 ? dt->hour % 12 : 12;
        snprintf(buf, n, "%d:%02d %s", h, dt->minute, dt->hour < 12 ? "AM" : "PM");
    } else {
        snprintf(buf, n, "%s %d", month_abbr[(dt->month + 11) % 12], dt->day);
    }
}

/* First line of a note, for its list row; the start of the file is enough. */
static void title_of(int i, char *buf, size_t n)
{
    char tmp[256];

    load_text(i, tmp, sizeof(tmp) - 1);
    size_t k = 0;
    const char *p = tmp;
    while (*p == '\n' || *p == ' ')
        p++;
    while (*p && *p != '\n' && k + 1 < n)
        buf[k++] = *p++;
    buf[k] = 0;
    if (!k)
        snprintf(buf, n, "New Note");
}

/* ---- list view ---------------------------------------------------------- */

static void draw_paper(gfx_surface *s, gfx_rect r, int rule_y0, int offset)
{
    gfx_fill(s, r, PAPER);
    int first = rule_y0 - offset;
    while (first < r.y)
        first += LINE_H;
    for (int y = first; y < r.y + r.h; y += LINE_H)
        gfx_fill(s, gfx_r(r.x, y, r.w, 1), RULE);
}

static char titles[MAX_NOTES][40];

static void draw_list(gfx_surface *s)
{
    char title[24], tbuf[16];
    gfx_rect body = gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H);
    const gfx_font *f = ui_font(FONT_B14), *tf = ui_font(FONT_R11);
    int row_h = LINE_H * 2 - 8;

    gfx_set_clip(s, body);
    gfx_fill(s, body, PAPER);
    for (int i = 0; i < nnotes || i * row_h < body.h + list_scroll.offset; i++) {
        int y = body.y + i * row_h - list_scroll.offset;
        if (y + row_h < body.y)
            continue;
        if (y > body.y + body.h)
            break;
        gfx_fill(s, gfx_r(0, y + row_h - 1, UI_W, 1), RULE);
        if (i >= nnotes)
            continue;
        if (i == pressed_row)
            gfx_vgradient(s, gfx_r(0, y, UI_W, row_h - 1), UI_SELECT_TOP, UI_SELECT_BOTTOM, 255);
        uint32_t ink = i == pressed_row ? 0xFFFFFF : INK;
        short_time(&notes[i].dt, tbuf, sizeof(tbuf));
        int tw = gfx_text_width(tf, tbuf);
        gfx_text_fit(s, MARGIN, y + (row_h - f->line_height) / 2, UI_W - MARGIN * 2 - tw - 24, f, titles[i], ink, 255);
        gfx_text(s, UI_W - 26 - tw, y + (row_h - tf->line_height) / 2 + 1, tf, tbuf,
                 i == pressed_row ? 0xFFFFFF : 0x8A7650, 255);
        ui_draw_chevron(s, UI_W - 16, y + row_h / 2 - 5, i == pressed_row ? 0xFFFFFF : 0x9A8A60);
    }
    gfx_reset_clip(s);

    snprintf(title, sizeof(title), "Notes (%d)", nnotes);
    ui_draw_navbar(s, title, NULL, NULL, NAV_STYLE);
    plus_rect = ui_draw_bar_button(s, "+", true, pressed_plus, false, NAV_STYLE);
}

static void refresh_titles(void)
{
    for (int i = 0; i < nnotes; i++)
        title_of(i, titles[i], sizeof(titles[i]));
    ui_scroll_reset(&list_scroll, nnotes * (LINE_H * 2 - 8), UI_H - UI_NAV_H);
}

/* ---- editor view -------------------------------------------------------- */

static int paper_h(void)
{
    return UI_H - UI_NAV_H - (editing ? UI_KB_H : TOOLBAR_H);
}

static int text_top(void)
{
    return UI_NAV_H + 30;           /* below the date line */
}

static void layout(void)
{
    nlines = ui_wrap(ui_font(FONT_R14), text, UI_W - MARGIN * 2, line_starts, 600);
    edit_scroll.content_h = 30 + nlines * LINE_H + 20;
    edit_scroll.view_h = paper_h();
}

static int cursor_line(void)
{
    int l = 0;

    while (l + 1 < nlines && line_starts[l + 1] <= cursor)
        l++;
    return l;
}

static void keep_cursor_visible(void)
{
    int y = 30 + cursor_line() * LINE_H;

    if (y - edit_scroll.offset < 0)
        edit_scroll.offset = y - 8;
    if (y + LINE_H - edit_scroll.offset > paper_h())
        edit_scroll.offset = y + LINE_H - paper_h() + 6;
    if (edit_scroll.offset < 0)
        edit_scroll.offset = 0;
}

static void draw_toolbar(gfx_surface *s)
{
    gfx_rect r = gfx_r(0, UI_H - TOOLBAR_H, UI_W, TOOLBAR_H);

    gfx_vgradient(s, gfx_r(0, r.y, UI_W, r.h / 2), 0xC79A6A, 0xA27243, 255);
    gfx_vgradient(s, gfx_r(0, r.y + r.h / 2, UI_W, r.h - r.h / 2), 0x93653A, 0x74471F, 255);
    gfx_fill(s, gfx_r(0, r.y, UI_W, 1), 0x3A220C);
    /* Trash can glyph. */
    int cx = UI_W / 2, cy = r.y + 8;
    uint32_t c = pressed_trash ? 0x3A220C : 0xFFFFFF;
    gfx_fill(s, gfx_r(cx - 7, cy + 2, 14, 2), c);
    gfx_fill(s, gfx_r(cx - 3, cy, 6, 2), c);
    gfx_fill(s, gfx_r(cx - 6, cy + 5, 12, 13), c);
    for (int i = -3; i <= 3; i += 3)
        gfx_fill(s, gfx_r(cx + i, cy + 7, 1, 9), 0x93653A);
    trash_rect = gfx_r(cx - 22, r.y, 44, r.h);
}

static void draw_edit(gfx_surface *s)
{
    char date[48];
    const gfx_font *f = ui_font(FONT_R14), *df = ui_font(FONT_R11);
    gfx_rect paper = gfx_r(0, UI_NAV_H, UI_W, paper_h());
    int off = edit_scroll.offset;

    layout();
    gfx_set_clip(s, paper);
    draw_paper(s, paper, UI_NAV_H + 30 + LINE_H - 3, off);
    gfx_fill(s, gfx_r(MARGIN - 4, paper.y, 1, paper.h), 0xE8A0A0);
    if (cur >= 0) {
        const struct gz_datetime *dt = &notes[cur].dt;
        int h = dt->hour % 12 ? dt->hour % 12 : 12;
        snprintf(date, sizeof(date), "%s %d   %d:%02d %s", month_abbr[(dt->month + 11) % 12], dt->day, h,
                 dt->minute, dt->hour < 12 ? "AM" : "PM");
        gfx_text(s, UI_W - MARGIN - gfx_text_width(df, date), paper.y + 8 - off, df, date, 0x8A7650, 255);
    }
    for (int l = 0; l < nlines; l++) {
        int y = text_top() + l * LINE_H - off;
        if (y + LINE_H < paper.y || y > paper.y + paper.h)
            continue;
        gfx_text_n(s, MARGIN, y + LINE_H - 4 - f->ascent, f, text + line_starts[l],
                   ui_line_len(text, line_starts, nlines, l), INK, 255);
    }
    if (editing) {
        int l = cursor_line();
        int x = MARGIN + gfx_text_width_n(f, text + line_starts[l], cursor - line_starts[l]);
        int y = text_top() + l * LINE_H - off;
        gfx_fill(s, gfx_r(x, y + 2, 2, LINE_H - 4), 0x3B6FD8);
    }
    gfx_reset_clip(s);

    ui_draw_navbar(s, NULL, "Notes", &back_rect, NAV_STYLE);
    if (editing)
        done_rect = ui_draw_bar_button(s, "Done", true, pressed_done, true, NAV_STYLE);
    else
        done_rect = gfx_r(0, 0, 0, 0);
    if (editing)
        ui_draw_keyboard(s, &kb, UI_H - UI_KB_H);
    else
        draw_toolbar(s);
    if (alert_open) {
        ui_draw_alert(s, "Delete Note", "This note will be deleted.", "Cancel", "Delete", &alert_cancel,
                      &alert_delete, alert_pressed);
    }
}

static void draw_current(gfx_surface *s)
{
    if (view == V_LIST)
        draw_list(s);
    else
        draw_edit(s);
}

static void redraw(void)
{
    draw_current(&ui.surf);
    ui_invalidate_all();
    ui_present();
}

static void open_note(int i, bool with_keyboard)
{
    cur = i;
    load_text(i, text, MAX_TEXT);
    cursor = (int)strlen(text);
    editing = with_keyboard;
    dirty = false;
    ui_keyboard_init(&kb);
    if (cursor)
        kb.shift = false;
    ui_scroll_reset(&edit_scroll, 0, 0);
    layout();
    if (editing)
        keep_cursor_visible();
    view = V_EDIT;
    ui_transition(draw_current, true);
}

static void new_note(void)
{
    if (nnotes >= MAX_NOTES)
        return;
    memmove(&notes[1], &notes[0], (size_t)nnotes * sizeof(struct meta));
    notes[0].id = next_id++;
    gz_rtc_get(&notes[0].dt);
    nnotes++;
    open_note(0, true);
}

static void back_to_list(void)
{
    if (cur >= 0 && !text[0]) {
        /* An empty note disappears on the way out. */
        delete_current();
    } else {
        save_current();
    }
    editing = false;
    view = V_LIST;
    refresh_titles();
    ui_transition(draw_current, false);
}

/* ---- editing ------------------------------------------------------------ */

static void insert_char(int ch)
{
    int len = (int)strlen(text);

    if (ch == '\b') {
        if (cursor > 0) {
            memmove(text + cursor - 1, text + cursor, (size_t)(len - cursor + 1));
            cursor--;
        }
    } else if (len < MAX_TEXT) {
        memmove(text + cursor + 1, text + cursor, (size_t)(len - cursor + 1));
        text[cursor++] = (char)ch;
    }
    dirty = true;
    last_edit = gz_time_ms();
    layout();
    keep_cursor_visible();
}

static void place_cursor(int x, int y)
{
    int line = (y - text_top() + edit_scroll.offset) / LINE_H;

    layout();
    if (line < 0)
        line = 0;
    cursor = ui_text_hit(ui_font(FONT_R14), text, line_starts, nlines, line, x - MARGIN);
}

/* ---- input -------------------------------------------------------------- */

static void list_event(const struct gz_input_event *e)
{
    int body_y = e->y - UI_NAV_H;
    int row_h = LINE_H * 2 - 8;

    if (e->type == GZ_EV_TOUCH_DOWN) {
        pressed_plus = gfx_rect_contains(plus_rect, e->x, e->y);
        pressed_row = -1;
        if (!pressed_plus && body_y >= 0) {
            ui_scroll_event(&list_scroll, e, body_y);
            int r = (body_y + list_scroll.offset) / row_h;
            if (r < nnotes && !list_scroll.moved)
                pressed_row = r;
        }
        redraw();
        return;
    }
    if (pressed_plus) {
        if (e->type == GZ_EV_TOUCH_UP) {
            bool hit = gfx_rect_contains(plus_rect, e->x, e->y);
            pressed_plus = false;
            if (hit)
                new_note();
            else
                redraw();
        }
        return;
    }
    if (ui_scroll_event(&list_scroll, e, body_y) || (list_scroll.moved && pressed_row >= 0)) {
        pressed_row = -1;
        redraw();
    }
    if (e->type == GZ_EV_TOUCH_UP) {
        int r = pressed_row;
        pressed_row = -1;
        if (r >= 0 && !list_scroll.moved)
            open_note(r, false);
        else
            redraw();
    }
}

static void edit_event(const struct gz_input_event *e)
{
    int kb_y = UI_H - UI_KB_H;

    if (alert_open) {
        int h = gfx_rect_contains(alert_cancel, e->x, e->y) ? 0
              : gfx_rect_contains(alert_delete, e->x, e->y) ? 1 : -1;
        if (e->type == GZ_EV_TOUCH_UP) {
            alert_open = false;
            alert_pressed = -1;
            if (h == 1) {
                delete_current();
                editing = false;
                view = V_LIST;
                refresh_titles();
                ui_transition(draw_current, false);
                return;
            }
        } else {
            alert_pressed = h;
        }
        redraw();
        return;
    }

    if (e->type == GZ_EV_TOUCH_DOWN) {
        in_keyboard = editing && e->y >= kb_y;
        pressed_done = editing && gfx_rect_contains(done_rect, e->x, e->y);
        pressed_trash = !editing && gfx_rect_contains(trash_rect, e->x, e->y);
    }
    if (in_keyboard) {
        bool changed;
        int ch = ui_keyboard_event(&kb, e, kb_y, &changed);
        if (ch)
            insert_char(ch);
        if (ch || changed)
            redraw();
        return;
    }
    if (e->type == GZ_EV_TOUCH_UP) {
        if (gfx_rect_contains(back_rect, e->x, e->y) && e->y < UI_NAV_H) {
            back_to_list();
            return;
        }
        if (pressed_done && gfx_rect_contains(done_rect, e->x, e->y)) {
            pressed_done = false;
            editing = false;
            save_current();
            redraw();
            return;
        }
        if (pressed_trash && gfx_rect_contains(trash_rect, e->x, e->y)) {
            pressed_trash = false;
            alert_open = true;
            redraw();
            return;
        }
        pressed_done = pressed_trash = false;
    }
    int py = e->y - UI_NAV_H;
    if (e->y >= UI_NAV_H && py < paper_h()) {
        bool moved = ui_scroll_event(&edit_scroll, e, py);
        if (e->type == GZ_EV_TOUCH_UP && !edit_scroll.moved) {
            place_cursor(e->x, e->y);
            if (!editing) {
                editing = true;
                kb.shift = !cursor || text[cursor - 1] == '\n';
                layout();
                keep_cursor_visible();
            }
            moved = true;
        }
        if (moved)
            redraw();
    } else if (e->type == GZ_EV_TOUCH_DOWN || e->type == GZ_EV_TOUCH_UP) {
        redraw();
    }
}

int main(void)
{
    ui_init(SB_STATUS_GRAY);
    load_index();
    refresh_titles();
    redraw();

    for (;;) {
        struct ui_scroll *sc = view == V_LIST ? &list_scroll : &edit_scroll;
        int timeout = sc->animating ? 30 : dirty ? SAVE_IDLE : -1;
        struct gz_input_event ev;

        if (ui_next_event(&ev, timeout)) {
            if (ev.type == GZ_EV_KEY_DOWN && ev.code == GZ_KEY_ESC && view == V_EDIT)
                back_to_list();
            else if (view == V_LIST)
                list_event(&ev);
            else
                edit_event(&ev);
        }
        if (ui_scroll_step(sc))
            redraw();
        if (dirty && gz_time_ms() - last_edit >= SAVE_IDLE)
            save_current();
    }
}
