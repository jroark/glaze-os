/* A WinCE message desktop presented as one SpringBoard surface. Original
 * application window procedures execute directly; built-in controls live here.
 */
#include "dialog_template.h"
#include "number.h"
#include "runtime.h"
#include <glaze/audio.h>

#define WINDOWS CE_WINDOWS
#define QUEUE 128
static struct ce_window windows[WINDOWS];
static struct {
    char name[64];
    CW proc;
} classes[24];
static unsigned class_count;
static struct ce_message queue[QUEUE];
static unsigned head, tail;
static CW focus, capture, modal;

static bool quitting;
static CW exit_code;
static uint8_t key_state[256];
static struct {
    CW window, id, modifiers, key;
} hotkeys[16];
struct ui_keyboard ce_keyboard;
bool ce_keyboard_open;
static struct {
    CW window, id, interval, deadline, proc;
} timers[16];
static CW builtin_proc(CW, CW, CW, CW);
static void draw_control(struct ce_window *);
static unsigned handle_index(CW handle)
{
    return handle - 0x70000000u;
}

struct ce_window *ce_window(CW handle)
{
    unsigned index = handle_index(handle);
    return index < WINDOWS && windows[index].handle ? &windows[index] : NULL;
}
bool ce_window_visible(CW handle)
{
    struct ce_window *w = ce_window(handle);
    for (unsigned depth = 0; w && depth < WINDOWS; depth++) {
        if (!w->visible)
            return false;
        if (!w->parent)
            return true;
        w = ce_window(w->parent);
    }
    return false;
}

gfx_rect ce_window_rect(CW handle)
{
    struct ce_window *w = ce_window(handle);
    if (!w)
        return gfx_r(0, 0, 240, 320);
    int x = w->rect.left, y = w->rect.top;
    struct ce_window *p = ce_window(w->parent);
    for (unsigned i = 0; p && i < WINDOWS; i++) {
        x += p->rect.left;
        y += p->rect.top;
        p = ce_window(p->parent);
    }
    return gfx_r(x, y, w->rect.right - w->rect.left, w->rect.bottom - w->rect.top);
}

void ce_post(CW window, CW message, CW wparam, CW lparam)
{
    if (head - tail >= QUEUE)
        ce_fatal("WinCE message queue is full");
    queue[head++ % QUEUE] = (struct ce_message){window, message, wparam, lparam, gz_time_ms(), 0, 0};
}

CW ce_send(CW handle, CW message, CW wparam, CW lparam)
{
    struct ce_window *w = ce_window(handle);
    if (!w)
        return 0;
    if (w->dialog_proc) {
        CW result = ce_call(w->dialog_proc, handle, message, wparam, lparam);
        if (result)
            return result;
        return builtin_proc(handle, message, wparam, lparam);
    }
    return ce_call(w->proc, handle, message, wparam, lparam);
}

static bool set_text(struct ce_window *w, const WCHAR *text)
{
    unsigned length = ce_wlen(text);
    if (length > 65535) {
        ce_error = 8;
        return false;
    }
    if (length + 1 > w->capacity) {
        WCHAR *copy = realloc(w->text, (length + 128u) * 2u);
        if (!copy) {
            ce_error = 8;
            return false;
        }
        w->text = copy;
        w->capacity = length + 128;
    }
    if (text)
        memmove(w->text, text, (length + 1) * 2u);
    else
        w->text[0] = 0;
    w->length = length;
    w->cursor = length;
    w->sel_start = w->sel_end = length;
    w->dirty = true;
    return true;
}

struct ce_window *ce_new_window(const WCHAR *klass, const WCHAR *text, CW style, int x, int y, int width,
                                int height, CW parent, CW id, CW proc, CW arg)
{
    unsigned i;
    for (i = 0; i < WINDOWS && windows[i].handle; i++)
        ;
    if (i == WINDOWS) {
        ce_error = 8;
        return NULL;
    }
    struct ce_window *w = &windows[i];
    memset(w, 0, sizeof(*w));
    w->handle = 0x70000000u + i;
    w->parent = parent;
    w->id = id;
    w->style = style;
    w->enabled = !(style & 0x08000000u);
    w->text_limit = 65535;
    if (WORD(klass) > 65535)
        ce_narrow(w->klass, sizeof(w->klass), klass);
    else
        strlcpy(w->klass, "Static", sizeof(w->klass));
    if (!proc)
        for (unsigned j = 0; j < class_count; j++)
            if (!strcasecmp(classes[j].name, w->klass)) {
                proc = classes[j].proc;
                break;
            }
    w->proc = proc ? proc : WORD(builtin_proc);
    if (!strcasecmp(w->klass, "msctls_updown32")) {
        w->spin_high = 100;
        if (style & 0x10)
            for (unsigned j = 0; j < i; j++)
                if (windows[j].handle && windows[j].parent == parent && !strcasecmp(windows[j].klass, "Edit"))
                    w->spin_buddy = windows[j].handle;
    }
    if (!strcasecmp(w->klass, "SysTabControl32")) {
        w->tabs = calloc(1, sizeof(*w->tabs));
        if (!w->tabs) {
            w->handle = 0;
            return NULL;
        }
        w->tabs->selected = -1;
    }
    if (x == (int)0x80000000u)
        x = 0;
    if (y == (int)0x80000000u)
        y = 0;
    if (width == (int)0x80000000u || width <= 0)
        width = 240;
    if (height == (int)0x80000000u || height <= 0)
        height = 320;
    width = MIN(width, 240);
    height = MIN(height, 320);
    w->rect = (struct ce_rect){x, y, x + width, y + height};
    if (!ce_list_init(w) || !set_text(w, text)) {
        ce_list_free(w);
        free(w->tabs);
        w->handle = 0;
        return NULL;
    }
    w->visible = (style & 0x10000000u) != 0;
    CW create[] = {arg,   ce_main.base, id,    parent,     (CW)height,  (CW)width,
                   (CW)y, (CW)x,        style, WORD(text), WORD(klass), 0};
    /* The callback can recursively create child windows. */
    if (ce_send(w->handle, 1, 0, WORD(create)) == 0xffffffffu) {
        free(w->text);
        free(w->tabs);
        memset(w, 0, sizeof(*w));
        return NULL;
    }
    ce_send(w->handle, 5, 0, (CW)width | (CW)height << 16);
    return w;
}

static void edited(struct ce_window *w)
{
    w->modified = w->dirty = true;
    ce_send(w->parent, 0x111, w->id | 0x3000000u, w->handle);
}

static void replace_selection(struct ce_window *w, const WCHAR *text)
{
    unsigned start = MIN(w->sel_start, w->sel_end), end = MAX(w->sel_start, w->sel_end), add = ce_wlen(text);
    start = MIN(start, w->length);
    end = MIN(end, w->length);
    unsigned length = w->length - (end - start) + add;
    if (length > w->text_limit)
        return;
    if (length + 1 > w->capacity) {
        WCHAR *copy = realloc(w->text, (length + 128) * 2);
        if (!copy)
            return;
        w->text = copy;
        w->capacity = length + 128;
    }
    memmove(w->text + start + add, w->text + end, (w->length - end + 1) * 2);
    if (add)
        memcpy(w->text + start, text, add * 2);
    w->length = length;
    w->cursor = start + add;
    w->sel_start = w->sel_end = w->cursor;
    edited(w);
}

static CW spin_proc(struct ce_window *w, CW msg, CW wp, CW lp)
{
    switch (msg) {
    case 0x469: {
        CW old = w->spin_buddy;
        w->spin_buddy = wp;
        return old;
    }
    case 0x46a:
        return w->spin_buddy;
    case 0x465:
        w->spin_high = (int16_t)lp;
        w->spin_low = (int16_t)(lp >> 16);
        return 0;
    case 0x466:
        return (w->spin_high & 65535) | (CW)w->spin_low << 16;
    case 0x46f:
        w->spin_low = (int32_t)wp;
        w->spin_high = (int32_t)lp;
        return 0;
    case 0x470:
        if (wp)
            *PTR(int32_t, wp) = w->spin_low;
        if (lp)
            *PTR(int32_t, lp) = w->spin_high;
        return 0;
    case 0x467:
    case 0x471:
    case 0x202: {
        struct ce_window *buddy = ce_window(w->spin_buddy);
        int32_t low = MIN(w->spin_low, w->spin_high), high = MAX(w->spin_low, w->spin_high);
        int32_t old = (int32_t)w->checked, value;
        if (msg == 0x202) {
            uint32_t n;
            if (buddy && ce_number_parse(buddy->text, true, &n))
                old = (int32_t)n;
            int delta = (int16_t)(lp >> 16) < (w->rect.bottom - w->rect.top) / 2 ? 1 : -1;
            if (w->spin_low > w->spin_high)
                delta = -delta;
            CW notification[] = {w->handle, w->id, (CW)-722, (CW)old, (CW)delta};
            if (ce_send(w->parent, 0x4e, w->id, WORD(notification)))
                return 0;
            delta = (int32_t)notification[4];
            int64_t next = (int64_t)old + delta;
            value = next < low ? low : next > high ? high : (int32_t)next;
        } else
            value = msg == 0x467 ? (int16_t)lp : (int32_t)lp;
        value = MAX(low, MIN(high, value));
        w->checked = (CW)value;
        if (buddy && (w->style & 2)) {
            WCHAR text[12];
            ce_number_format(text, value, true);
            set_text(buddy, text);
            if (msg == 0x202)
                edited(buddy);
        }
        return msg == 0x202 ? 0 : msg == 0x467 ? old & 65535 : (CW)old;
    }
    case 0x468:
    case 0x472: {
        struct ce_window *buddy = ce_window(w->spin_buddy);
        uint32_t value = w->checked;
        bool ok = !buddy || !(w->style & 2) || ce_number_parse(buddy->text, true, &value);
        if (msg == 0x472 && lp)
            *PTR(CW, lp) = !ok;
        return msg == 0x472 ? value : (value & 65535) | (!ok ? 0x10000u : 0);
    }
    default:
        return 0;
    }
}
static CW builtin_proc(CW handle, CW message, CW wp, CW lp)
{
    struct ce_window *w = ce_window(handle);
    if (!w)
        return 0;
    if (!strcasecmp(w->klass, "msctls_updown32") &&
        (message == 0x202 || (message >= 0x465 && message <= 0x472)))
        return spin_proc(w, message, wp, lp);
    CW list_result;
    if (ce_list_message(w, message, wp, lp, &list_result))
        return list_result;
    if (w->tabs) {
        struct ce_tabs *tabs = w->tabs;
        switch (message) {
        case 0x1304: /* TCM_GETITEMCOUNT */
            return tabs->count;
        case 0x133e: { /* TCM_INSERTITEMW */
            if (!lp || tabs->count == ARRAY_SIZE(tabs->text))
                return 0xffffffffu;
            unsigned index = MIN(wp, tabs->count);
            memmove(tabs->text[index + 1], tabs->text[index], (tabs->count - index) * sizeof(tabs->text[0]));
            const CW *item = PTR(CW, lp);
            unsigned n = (item[0] & 1) ? MIN(ce_wlen(PTR(WCHAR, item[3])), 63u) : 0;
            if (n)
                memcpy(tabs->text[index], PTR(void, item[3]), n * 2);
            tabs->text[index][n] = 0;
            tabs->count++;
            if (tabs->selected < 0)
                tabs->selected = 0;
            else if ((unsigned)tabs->selected >= index)
                tabs->selected++;
            w->dirty = true;
            return index;
        }
        case 0x130b: /* TCM_GETCURSEL */
            return (CW)tabs->selected;
        case 0x130c: { /* TCM_SETCURSEL; no WM_NOTIFY for programmatic changes. */
            int previous = tabs->selected;
            if (wp != 0xffffffffu && wp >= tabs->count)
                return 0xffffffffu;
            tabs->selected = (int)wp;
            w->dirty = true;
            return (CW)previous;
        }
        case 0x1328: { /* TCM_ADJUSTRECT */
            if (!lp)
                return 0;
            struct ce_rect *r = PTR(struct ce_rect, lp);
            int sign = wp ? -1 : 1;
            r->left += sign * 2;
            r->top += sign * 24;
            r->right -= sign * 2;
            r->bottom -= sign * 2;
            return 0;
        }
        case 0x202: {
            int x = (int16_t)lp, y = (int16_t)(lp >> 16);
            if (x < 0 || y < 0 || y >= 24 || !tabs->count)
                return 0;
            unsigned index = (unsigned)x / MAX(1, (w->rect.right - w->rect.left) / (int)tabs->count);
            if (index >= tabs->count || (int)index == tabs->selected)
                return 0;
            CW notification[3] = {handle, w->id, (CW)-552};
            if (ce_send(w->parent, 0x4e, w->id, WORD(notification)))
                return 0;
            tabs->selected = (int)index;
            w->dirty = true;
            notification[2] = (CW)-551;
            ce_send(w->parent, 0x4e, w->id, WORD(notification));
            return 0;
        }
        }
    }
    switch (message) {
    case 0xf:
        draw_control(w);
        return 0;
    case 0xc:
        return set_text(w, PTR(WCHAR, lp));
    case 0xd: {
        unsigned n = wp ? MIN(w->length, wp - 1) : 0;
        if (wp && lp) {
            memcpy(PTR(void, lp), w->text, n * 2);
            PTR(WCHAR, lp)[n] = 0;
        }
        return n;
    }
    case 0xe:
        return w->length;
    case 0x81:
        return 1;
    case 0x10:
        ce_send(handle, 2, 0, 0);
        w->visible = false;
        return 0;
    case 0x30:
        w->dirty = true;
        return 0;
    case 0xf0: /* BM_GETCHECK */
        return w->checked;
    case 0xf1: /* BM_SETCHECK */
        w->checked = MIN(wp, 2u);
        w->dirty = true;
        return 0;
    case 0xc5: /* EM_SETLIMITTEXT */
        w->text_limit = wp ? MIN(wp, 65535u) : 65535;
        return 0;
    case 0xcf: /* EM_SETREADONLY */
        w->style = wp ? w->style | 0x800u : w->style & ~0x800u;
        return 1;
    case 0xb0:
        if (wp)
            *PTR(CW, wp) = w->sel_start;
        if (lp)
            *PTR(CW, lp) = w->sel_end;
        return (w->sel_start & 65535) | w->sel_end << 16;
    case 0xb1:
        w->sel_start = wp == 0xffffffffu ? w->cursor : MIN(wp, w->length);
        w->sel_end = lp == 0xffffffffu ? w->length : MIN(lp, w->length);
        if (wp == 0xffffffffu)
            w->sel_start = w->sel_end = w->cursor;
        w->cursor = w->sel_end;
        w->dirty = true;
        return 0;
    case 0xb8:
        return w->modified;
    case 0xb9:
        w->modified = wp != 0;
        return 0;
    case 0xba: {
        CW lines = 1;
        for (unsigned i = 0; i < w->length; i++)
            if (w->text[i] == '\n')
                lines++;
        return lines;
    }
    case 0xbb: {
        CW line = 0;
        for (unsigned i = 0; i < w->length; i++) {
            if (line == wp)
                return i;
            if (w->text[i] == '\n')
                line++;
        }
        return wp == line ? w->length : 0xffffffffu;
    }
    case 0xc1: {
        unsigned p = MIN(wp, w->length), start = p;
        while (start && w->text[start - 1] != '\n')
            start--;
        while (p < w->length && w->text[p] != '\n')
            p++;
        return p - start;
    }
    case 0xc2:
        replace_selection(w, PTR(WCHAR, lp));
        return 1;
    case 0x102:
        if (strcasecmp(w->klass, "Edit") || (w->style & 0x800))
            return 0;
        if (wp == 8) {
            if (w->sel_start == w->sel_end && w->cursor)
                w->sel_start = w->cursor - 1;
            static WCHAR empty[1];
            replace_selection(w, empty);
        } else if (wp >= 32 || wp == 13 || wp == 10 || wp == 9) {
            WCHAR text[3] = {(WCHAR)wp, 0, 0};
            if (wp == 13) {
                text[0] = '\r';
                text[1] = '\n';
            }
            replace_selection(w, text);
        }
        return 0;
    case 0x100:
        if (wp == 0x25 && w->cursor)
            w->cursor--;
        if (wp == 0x27 && w->cursor < w->length)
            w->cursor++;
        if (wp == 0x24) {
            while (w->cursor && w->text[w->cursor - 1] != '\n')
                w->cursor--;
        }
        if (wp == 0x23) {
            while (w->cursor < w->length && w->text[w->cursor] != '\n')
                w->cursor++;
        }
        if (wp == 0x2e) {
            if (w->style & 0x800)
                return 0;
            if (w->sel_start == w->sel_end)
                w->sel_end = MIN(w->length, w->cursor + 1);
            static WCHAR empty[1];
            replace_selection(w, empty);
        } else
            w->sel_start = w->sel_end = w->cursor;
        w->dirty = true;
        return 0;
    default:
        return 0;
    }
}

static void draw_control(struct ce_window *w)
{
    gfx_rect r = ce_window_rect(w->handle);
    if (w->dialog_proc) {
        gfx_fill(&ui.surf, r, 0xc0c0c0);
        if (w->property_page)
            return;
        char title[256];
        ce_narrow(title, sizeof(title), w->text);
        gfx_fill(&ui.surf, gfx_r(r.x, r.y, r.w, 24), 0x305080);
        gfx_text(&ui.surf, r.x + 3, r.y + 4, ui_font(FONT_R12), title, 0xffffff, 255);
    } else if (w->list) {
        ce_list_draw(w);
    } else if (w->tabs) {
        gfx_fill(&ui.surf, r, 0xc0c0c0);
        int width = r.w / MAX(1u, w->tabs->count);
        for (unsigned i = 0; i < w->tabs->count; i++) {
            char text[192];
            ce_narrow(text, sizeof(text), w->tabs->text[i]);
            gfx_rect tab = gfx_r(r.x + (int)i * width, r.y, width, 24);
            gfx_fill(&ui.surf, tab, (int)i == w->tabs->selected ? 0xeeeeee : 0xaaaaaa);
            gfx_text_center(&ui.surf, tab.x, tab.y + 4, tab.w, ui_font(FONT_R12), text, 0, 255);
        }
    } else if (!strcasecmp(w->klass, "Edit")) {
        gfx_set_clip(&ui.surf, r);
        gfx_fill(&ui.surf, r, 0xffffff);
        int x = r.x + 2, y = r.y + 2;
        for (unsigned i = 0; i <= w->length; i++) {
            if (i == w->cursor && focus == w->handle)
                gfx_fill(&ui.surf, gfx_r(x, y, 1, 14), 0x000000);
            if (i == w->length)
                break;
            WCHAR ch = w->text[i];
            if (ch == '\r')
                continue;
            if (ch == '\n' || x > r.x + r.w - 8) {
                x = r.x + 2;
                y += 14;
                if (ch == '\n')
                    continue;
            }
            char s[2] = {(char)(ch < 256 ? ch : '?'), 0};
            x += gfx_text(&ui.surf, x, y, ui_font(FONT_R12), s, 0, 255);
        }
        gfx_reset_clip(&ui.surf);
    } else if (!strcasecmp(w->klass, "Button") || !strcasecmp(w->klass, "CSOButton")) {
        char text[128];
        ce_narrow(text, sizeof(text), w->text);
        if ((w->style & 15) == 7) {
            gfx_line(&ui.surf, r.x, r.y + 6, r.x + r.w - 1, r.y + 6, 0x808080, 255);
            gfx_line(&ui.surf, r.x, r.y + 6, r.x, r.y + r.h - 1, 0x808080, 255);
            gfx_line(&ui.surf, r.x + r.w - 1, r.y + 6, r.x + r.w - 1, r.y + r.h - 1, 0x808080, 255);
            gfx_line(&ui.surf, r.x, r.y + r.h - 1, r.x + r.w - 1, r.y + r.h - 1, 0x808080, 255);
            gfx_text(&ui.surf, r.x + 4, r.y, ui_font(FONT_R12), text, 0, 255);
            return;
        }
        gfx_fill(&ui.surf, r, 0xc0c0c0);
        if ((w->style & 15) == 2 || (w->style & 15) == 3) {
            gfx_fill(&ui.surf, gfx_r(r.x + 1, r.y + 3, 13, 13), 0xffffff);
            if (w->checked)
                gfx_text(&ui.surf, r.x + 2, r.y + 2, ui_font(FONT_B12), "x", 0, 255);
            struct ce_rect label = {18, 0, r.w, r.h};
            ce_draw_text(w->handle, WORD(w->text), 0xffffffffu, WORD(&label), 0x10);
        } else {
            gfx_line(&ui.surf, r.x, r.y, r.x + r.w - 1, r.y, 0x808080, 255);
            gfx_text_center(&ui.surf, r.x, r.y + 3, r.w, ui_font(FONT_R12), text, 0, 255);
        }
    } else if (!strcasecmp(w->klass, "msctls_updown32")) {
        gfx_fill(&ui.surf, r, 0xaaaaaa);
        gfx_text_center(&ui.surf, r.x, r.y - 2, r.w, ui_font(FONT_R12), "+", 0, 255);
        gfx_text_center(&ui.surf, r.x, r.y + r.h / 2 - 2, r.w, ui_font(FONT_R12), "-", 0, 255);
    } else if (!strcasecmp(w->klass, "Static")) {
        if ((w->style & 31) == 3)
            return; /* Resource icon, no text. */
        struct ce_rect label = {0, 0, r.w, r.h};
        unsigned align = (w->style & 31) == 1 ? 1 : (w->style & 31) == 2 ? 2 : 0;
        ce_draw_text(w->handle, WORD(w->text), (CW)-1, WORD(&label), 0x10 | align);
    } else if (!w->parent)
        gfx_fill(&ui.surf, r, 0xffffff);
}

void ce_redraw_all(void)
{
    for (unsigned i = 0; i < WINDOWS; i++)
        if (windows[i].handle)
            windows[i].dirty = true;
}

void ce_render(void)
{
    for (unsigned i = 0; i < WINDOWS; i++) {
        struct ce_window *w = &windows[i];
        if (!w->handle || !ce_window_visible(w->handle) || !w->dirty)
            continue;
        w->dirty = false;
        if (w->proc == WORD(builtin_proc))
            draw_control(w);
        else
            ce_send(w->handle, 0xf, 0, 0);
        /* Parent painting shares this surface with its children. Repaint
         * children afterwards so a parent's background fill cannot erase
         * controls that had no invalidation of their own. */
        for (unsigned j = 0; j < WINDOWS; j++)
            if (windows[j].handle && windows[j].parent == w->handle)
                windows[j].dirty = true;
    }
    if (ce_keyboard_open && !ui.hardware_keyboard)
        ui_draw_keyboard(&ui.surf, &ce_keyboard, 320 - UI_KB_H);
    ce_shell_draw();
    ce_list_popup_draw();
    if (modal) {
        for (unsigned i = 0; i < WINDOWS; i++) {
            struct ce_window *w = &windows[i];
            struct ce_window *p = w;
            while (p && p->handle != modal)
                p = ce_window(p->parent);
            if (p && w->handle && ce_window_visible(w->handle)) {
                if (w->proc == WORD(builtin_proc))
                    draw_control(w);
                else
                    ce_send(w->handle, 0xf, 0, 0);
            }
        }
        if (ce_keyboard_open && !ui.hardware_keyboard)
            ui_draw_keyboard(&ui.surf, &ce_keyboard, 320 - UI_KB_H);
    }
    ui_invalidate_all();
    ui_present();
}

static void input(const struct gz_input_event *e)
{
    if (ce_list_popup_input(e))
        return;
    if (!modal && ce_shell_input(e)) {
        return;
    }
    if (e->type == GZ_EV_TEXT) {
        if (focus)
            ce_post(focus, 0x102, e->code, 0);
        return;
    }
    if (e->type == GZ_EV_KEY_DOWN || e->type == GZ_EV_KEY_UP) {
        CW vk = 0;
        switch (e->code) {
        case GZ_KEY_OK:
        case GZ_KEY_ENTER:
            vk = 13;
            break;
        case GZ_KEY_ESC:
            vk = 27;
            break;
        case GZ_KEY_UP:
            vk = 0x26;
            break;
        case GZ_KEY_DOWN:
            vk = 0x28;
            break;
        case GZ_KEY_LEFT:
            vk = 0x25;
            break;
        case GZ_KEY_RIGHT:
            vk = 0x27;
            break;
        case GZ_KEY_BACKSPACE:
            vk = 8;
            break;
        case GZ_KEY_DELETE:
            vk = 0x2e;
            break;
        case GZ_KEY_TAB:
            vk = 9;
            break;
        case GZ_KEY_KBD_HOME:
            vk = 0x24;
            break;
        case GZ_KEY_END:
            vk = 0x23;
            break;
        default:
            break;
        }
        if (vk < 256)
            key_state[vk] = e->type == GZ_EV_KEY_DOWN;
        if (e->type == GZ_EV_KEY_DOWN)
            for (unsigned i = 0; i < ARRAY_SIZE(hotkeys); i++)
                if (hotkeys[i].window && hotkeys[i].key == vk && !hotkeys[i].modifiers) {
                    ce_post(hotkeys[i].window, 0x312, hotkeys[i].id, vk << 16);
                    return;
                }
        if (vk && focus)
            ce_post(focus, e->type == GZ_EV_KEY_DOWN ? 0x100 : 0x101, vk, 0);
        return;
    }
    if (e->type == GZ_EV_TOUCH_CANCEL) {
        /* Home mid-touch: as on Windows CE, WM_CANCELMODE ends a capture
         * without the button-up that would click. */
        bool changed = false;
        ui_keyboard_event(&ce_keyboard, e, 320 - UI_KB_H, &changed);
        if (capture)
            ce_post(capture, 0x1F, 0, 0);
        capture = 0;
        if (changed)
            ce_render();
        return;
    }
    if (e->type != GZ_EV_TOUCH_DOWN && e->type != GZ_EV_TOUCH_MOVE && e->type != GZ_EV_TOUCH_UP)
        return;
    if (ce_keyboard_open && !ui.hardware_keyboard && e->y >= 320 - UI_KB_H) {
        bool changed = false;
        int c = ui_keyboard_event(&ce_keyboard, e, 320 - UI_KB_H, &changed);
        if (c && focus)
            ce_post(focus, 0x102, c, 0);
        if (changed)
            ce_render();
        return;
    }
    struct ce_window *w = ce_window(capture);
    if (!w)
        for (unsigned i = 0; i < WINDOWS; i++)
            if (windows[i].handle && ce_window_visible(windows[i].handle) && windows[i].enabled &&
                (strcasecmp(windows[i].klass, "Button") || (windows[i].style & 15) != 7) &&
                gfx_rect_contains(ce_window_rect(windows[i].handle), e->x, e->y))
                w = &windows[i];
    if (!w)
        return;
    if (modal) {
        struct ce_window *p = w;
        while (p && p->handle != modal)
            p = ce_window(p->parent);
        if (!p)
            return;
    }
    gfx_rect r = ce_window_rect(w->handle);
    if (e->type == GZ_EV_TOUCH_DOWN) {
        focus = w->handle;
        if (!strcasecmp(w->klass, "Edit") && !(w->style & 0x800)) {
            ce_keyboard_open = true;
            w->dirty = true;
        }
    }
    CW msg = e->type == GZ_EV_TOUCH_DOWN ? 0x201 : e->type == GZ_EV_TOUCH_UP ? 0x202 : 0x200;
    ce_post(w->handle, msg, e->type == GZ_EV_TOUCH_UP ? 0 : 1,
            (uint16_t)(e->x - r.x) | (CW)(uint16_t)(e->y - r.y) << 16);
    if (e->type == GZ_EV_TOUCH_UP &&
        (!strcasecmp(w->klass, "Button") || !strcasecmp(w->klass, "CSOButton"))) {
        if ((w->style & 15) == 3)
            w->checked ^= 1;
        w->dirty = true;
        ce_post(w->parent, 0x111, w->id, w->handle);
    }
}

CE_API(RegisterClassW)
{
    if (class_count == ARRAY_SIZE(classes))
        return 0;
    uint8_t *p = PTR(uint8_t, a0);
    ce_narrow(classes[class_count].name, sizeof(classes[0].name), PTR(WCHAR, ce_u32(p + 36)));
    classes[class_count].proc = ce_u32(p + 4);
    return ++class_count;
}
CE_API(CreateWindowExW)
{
    struct ce_window *w =
        ce_new_window(PTR(WCHAR, a1), PTR(WCHAR, a2), a3, (int)a4, (int)a5, (int)a6, (int)a7, a8, a9, 0, a11);
    return w ? w->handle : 0;
}
CE_API(DefWindowProcW)
{
    return builtin_proc(a0, a1, a2, a3);
}
CE_API(CallWindowProcW)
{
    return ce_call(a0, a1, a2, a3, a4);
}
CE_API(SendMessageW)
{
    return ce_send(a0, a1, a2, a3);
}
CE_API(PostMessageW)
{
    ce_post(a0, a1, a2, a3);
    return 1;
}
CE_API(PostQuitMessage)
{
    quitting = true;
    exit_code = a0;
    return 0;
}
CE_API(GetMessageW)
{
    for (;;) {
        if (quitting) {
            memset(PTR(void, a0), 0, sizeof(struct ce_message));
            PTR(struct ce_message, a0)->message = 0x12;
            PTR(struct ce_message, a0)->wparam = exit_code;
            return 0;
        }
        if (tail != head) {
            struct ce_message m = queue[tail++ % QUEUE];
            *PTR(struct ce_message, a0) = m;
            return m.message != 0x12;
        }
        ce_render();
        CW now = gz_time_ms();
        for (unsigned i = 0; i < ARRAY_SIZE(timers); i++)
            if (timers[i].id && (int32_t)(now - timers[i].deadline) >= 0) {
                timers[i].deadline = now + timers[i].interval;
                ce_post(timers[i].window, 0x113, timers[i].id, timers[i].proc);
            }
        if (tail != head)
            continue;
        struct gz_input_event e;
        if (ui_next_event(&e, 20))
            input(&e);
    }
}
CE_API(TranslateMessage)
{
    struct ce_message *m = PTR(struct ce_message, a0);
    if (m->message == 0x100 && (m->wparam == 8 || m->wparam == 13 || m->wparam == 9))
        ce_post(m->window, 0x102, m->wparam, 0);
    return 1;
}
CE_API(DispatchMessageW)
{
    struct ce_message *m = PTR(struct ce_message, a0);
    if (m->message == 0x113 && m->lparam)
        return ce_call(m->lparam, m->window, 0x113, m->wparam, gz_time_ms());
    return ce_send(m->window, m->message, m->wparam, m->lparam);
}
CE_API(EndDialog)
{
    struct ce_window *w = ce_window(a0);
    if (!w || !w->dialog_proc)
        return 0;
    w->dialog_done = true;
    w->dialog_result = a1;
    return 1;
}
CE_API(DialogBoxIndirectParamW)
{
    /* Loaded resource templates must stay within the mapped PE. */
    if (a1 < ce_main.base || a1 - ce_main.base >= ce_main.pe.size || !a3) {
        ce_error = 87;
        return (CW)-1;
    }
    struct ce_dialog_template *d = malloc(sizeof(*d));
    if (!d)
        return (CW)-1;
    if (!ce_dialog_decode(d, PTR(void, a1), ce_main.pe.size - (a1 - ce_main.base))) {
        free(d);
        ce_error = 50;
        return (CW)-1;
    }
    static const WCHAR klass[] = {'_', 'c', 'e', '_', 'd', 'i', 'a', 'l', 'o', 'g', 0};
    int width = MIN(240, d->width * 7 / 4), height = MIN(300, d->height * 14 / 8 + 54);
    struct ce_window *w = ce_new_window(klass, d->title, d->style | 0x10000000u, (240 - width) / 2, 28, width,
                                        height, 0, 0, 0, 0);
    if (!w) {
        free(d);
        return (CW)-1;
    }
    w->dialog_proc = a3;
    CW handle = w->handle;
    CW first = ce_dialog_controls(handle, d, 24);
    free(d);
    if (first == 0xffffffffu) {
        ce_destroy_window(handle);
        return (CW)-1;
    }
    static const WCHAR button[] = {'B', 'u', 't', 't', 'o', 'n', 0};
    static const WCHAR yes[] = {'O', 'K', 0}, no[] = {'C', 'a', 'n', 'c', 'e', 'l', 0};
    bool has_ok = false, has_cancel = false;
    for (unsigned i = 0; i < WINDOWS; i++)
        if (windows[i].handle && windows[i].parent == handle) {
            has_ok |= windows[i].id == 1;
            has_cancel |= windows[i].id == 2;
        }
    if ((!has_ok &&
         !ce_new_window(button, yes, 0x50000001u, width - 120, height - 26, 54, 24, handle, 1, 0, 0)) ||
        (!has_cancel &&
         !ce_new_window(button, no, 0x50000000u, width - 64, height - 26, 62, 24, handle, 2, 0, 0))) {
        ce_destroy_window(handle);
        return (CW)-1;
    }
    CW old_focus = focus;
    focus = first ? first : handle;
    ce_call(a3, handle, 0x110, first, a4);
    CW result = ce_modal_run(handle);
    ce_destroy_window(handle);
    focus = old_focus;
    ce_redraw_all();
    return result;
}
CW ce_dialog_controls(CW handle, const struct ce_dialog_template *d, int top)
{
    CW first = 0;
    for (unsigned i = 0; i < d->count; i++) {
        const struct ce_dialog_item *item = &d->items[i];
        struct ce_window *child =
            ce_new_window(item->klass, item->text, item->style, item->x * 7 / 4, top + item->y * 14 / 8,
                          MAX(1, item->width * 7 / 4), MAX(1, item->height * 14 / 8), handle, item->id, 0, 0);
        if (!child)
            return (CW)-1;
        if (!first && (item->style & 0x10000u) && child->enabled)
            first = child->handle;
    }
    return first;
}
CW ce_modal_run(CW handle)
{
    struct ce_window *w = ce_window(handle);
    if (!w)
        return (CW)-1;
    CW old_focus = focus, old_modal = modal;
    bool old_keyboard = ce_keyboard_open;
    if (!ce_window(focus) || !ce_window_visible(focus))
        focus = handle;
    modal = handle;
    capture = 0;
    ce_keyboard_open = false;
    while (w->handle && !w->dialog_done && !quitting) {
        struct ce_message m;
        ce_GetMessageW(WORD(&m), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        if (quitting)
            break;
        if (m.message == 0x100 && (m.wparam == 13 || m.wparam == 27)) {
            ce_send(handle, 0x111, m.wparam == 13 ? 1 : 2, 0);
        } else if (m.message == 0x100 && m.wparam == 9) {
            unsigned start = handle_index(focus);
            for (unsigned n = 1; n <= WINDOWS; n++) {
                struct ce_window *child = &windows[(start + n) % WINDOWS];
                struct ce_window *ancestor = child;
                for (unsigned depth = 0; ancestor && ancestor->handle != handle && depth < WINDOWS; depth++)
                    ancestor = ce_window(ancestor->parent);
                if (child->handle && ce_window_visible(child->handle) && child->enabled &&
                    ancestor && ancestor->handle == handle && (child->style & 0x10000u)) {
                    focus = child->handle;
                    break;
                }
            }
        } else {
            ce_TranslateMessage(WORD(&m), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            ce_DispatchMessageW(WORD(&m), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
    }
    CW result = w->dialog_result;
    modal = old_modal;
    focus = old_focus;
    ce_keyboard_open = old_keyboard;
    ce_redraw_all();
    return result;
}

CE_API(MessageBeep)
{
    audio_sound(AUDIO_SND_NOTIFY);
    return 1;
}
CE_API(ShowWindow)
{
    struct ce_window *w = ce_window(a0);
    if (!w)
        return 0;
    bool was = w->visible;
    w->visible = a1 != 0;
    w->dirty = true;
    struct ce_window *parent = ce_window(w->parent);
    if (parent)
        parent->dirty = true;
    if (!w->parent)
        focus = w->handle;
    return was;
}
CE_API(UpdateWindow)
{
    struct ce_window *w = ce_window(a0);
    if (!w)
        return 0;
    w->dirty = true;
    ce_render();
    return 1;
}
CE_API(InvalidateRect)
{
    struct ce_window *w = ce_window(a0);
    if (!w)
        return 0;
    w->dirty = true;
    return 1;
}
CE_API(GetClientRect)
{
    struct ce_window *w = ce_window(a0);
    if (!w || !a1)
        return 0;
    *PTR(struct ce_rect, a1) =
        (struct ce_rect){0, 0, w->rect.right - w->rect.left, w->rect.bottom - w->rect.top};
    return 1;
}
CE_API(GetWindowRect)
{
    if (!ce_window(a0) || !a1)
        return 0;
    gfx_rect r = ce_window_rect(a0);
    *PTR(struct ce_rect, a1) = (struct ce_rect){r.x, r.y, r.x + r.w, r.y + r.h};
    return 1;
}
CE_API(MoveWindow)
{
    struct ce_window *w = ce_window(a0);
    if (!w)
        return 0;
    w->rect = (struct ce_rect){(int)a1, (int)a2, (int)(a1 + a3), (int)(a2 + a4)};
    w->dirty = true;
    ce_send(a0, 5, 0, (a3 & 65535) | a4 << 16);
    return 1;
}
CE_API(ClientToScreen)
{
    if (!ce_window(a0))
        return 0;
    gfx_rect r = ce_window_rect(a0);
    int32_t *p = PTR(int32_t, a1);
    p[0] += r.x;
    p[1] += r.y;
    return 1;
}
CE_API(FindWindowW)
{
    char klass[64];
    ce_narrow(klass, sizeof(klass), PTR(WCHAR, a0));
    for (unsigned i = 0; i < WINDOWS; i++)
        if (windows[i].handle && (!a0 || !strcasecmp(windows[i].klass, klass)) &&
            (!a1 || !ce_wcmp(windows[i].text, PTR(WCHAR, a1))))
            return windows[i].handle;
    return 0;
}
CE_API(GetWindowLongW)
{
    struct ce_window *w = ce_window(a0);
    if (!w)
        return 0;
    switch ((int)a1) {
    case -4:
        return w->proc;
    case -12:
        return w->id;
    case -16:
        return w->style;
    case 0:
        return w->property_page ? w->dialog_message_result : w->user;
    case 8:
    case -21:
        return w->user;
    default:
        return 0;
    }
}
CE_API(SetWindowLongW)
{
    struct ce_window *w = ce_window(a0);
    if (!w)
        return 0;
    CW *p = NULL;
    switch ((int)a1) {
    case -4:
        p = &w->proc;
        break;
    case -12:
        p = &w->id;
        break;
    case -16:
        p = &w->style;
        break;
    case 0:
        p = w->property_page ? &w->dialog_message_result : &w->user;
        break;
    case 8:
    case -21:
        p = &w->user;
        break;
    default:
        ce_error = 87;
        return 0;
    }
    CW old = *p;
    *p = a2;
    return old;
}
CE_API(GetParent)
{
    struct ce_window *w = ce_window(a0);
    return w ? w->parent : 0;
}
CE_API(GetDlgItem)
{
    for (unsigned i = 0; i < WINDOWS; i++)
        if (windows[i].handle && windows[i].parent == a0 && windows[i].id == a1)
            return windows[i].handle;
    return 0;
}
CE_API(GetWindowTextW)
{
    return ce_send(a0, 0xd, a2, a1);
}
CE_API(GetWindowTextLengthW)
{
    return ce_send(a0, 0xe, 0, 0);
}
CE_API(SetWindowTextW)
{
    struct ce_window *w = ce_window(a0);
    return w && set_text(w, PTR(WCHAR, a1));
}
CE_API(SetDlgItemTextW)
{
    for (unsigned i = 0; i < WINDOWS; i++)
        if (windows[i].handle && windows[i].parent == a0 && windows[i].id == a1)
            return set_text(&windows[i], PTR(WCHAR, a2));
    return 0;
}
CE_API(GetDlgItemTextW)
{
    for (unsigned i = 0; i < WINDOWS; i++)
        if (windows[i].handle && windows[i].parent == a0 && windows[i].id == a1)
            return ce_send(windows[i].handle, 0xd, a3, a2);
    return 0;
}
CE_API(SetDlgItemInt)
{
    WCHAR text[12];
    ce_number_format(text, a2, a3 != 0);
    for (unsigned i = 0; i < WINDOWS; i++)
        if (windows[i].handle && windows[i].parent == a0 && windows[i].id == a1)
            return set_text(&windows[i], text);
    return 0;
}
CE_API(GetDlgItemInt)
{
    uint32_t value = 0;
    bool ok = false;
    for (unsigned i = 0; i < WINDOWS; i++)
        if (windows[i].handle && windows[i].parent == a0 && windows[i].id == a1) {
            ok = ce_number_parse(windows[i].text, a3 != 0, &value);
            break;
        }
    if (a2)
        *PTR(CW, a2) = ok;
    return ok ? value : 0;
}
CE_API(SetFocus)
{
    CW old = focus;
    if (ce_window(a0)) {
        focus = a0;
        ce_send(old, 8, a0, 0);
        ce_send(a0, 7, old, 0);
    }
    return old;
}
CE_API(SetForegroundWindow)
{
    if (!ce_window(a0))
        return 0;
    focus = a0;
    return 1;
}
CE_API(GetActiveWindow)
{
    struct ce_window *w = ce_window(focus);
    while (w && w->parent)
        w = ce_window(w->parent);
    return w ? w->handle : 0;
}
CE_API(GetFocus)
{
    return focus;
}
CE_API(IsWindowVisible)
{
    return ce_window_visible(a0);
}
CE_API(IsWindowEnabled)
{
    struct ce_window *w = ce_window(a0);
    for (unsigned depth = 0; w && depth < WINDOWS; depth++) {
        if (!w->enabled)
            return 0;
        if (!w->parent)
            return 1;
        w = ce_window(w->parent);
    }
    return 0;
}
CE_API(OffsetRect)
{
    if (!a0)
        return 0;
    struct ce_rect *r = PTR(struct ce_rect, a0);
    r->left += (int)a1;
    r->right += (int)a1;
    r->top += (int)a2;
    r->bottom += (int)a2;
    return 1;
}
CE_API(InitCommonControls)
{
    /* Built-in controls are available from process startup. */
    return 0;
}
CE_API(InitCommonControlsEx)
{
    if (!a0 || ce_u32(PTR(void, a0)) != 8) {
        ce_error = 87;
        return 0;
    }
    return 1;
}
CE_API(SetRect)
{
    if (!a0)
        return 0;
    *PTR(struct ce_rect, a0) = (struct ce_rect){(int)a1, (int)a2, (int)a3, (int)a4};
    return 1;
}
CE_API(SetWindowPos)
{
    struct ce_window *w = ce_window(a0);
    if (!w)
        return 0;
    int width = w->rect.right - w->rect.left, height = w->rect.bottom - w->rect.top;
    if (!(a6 & 2)) {
        w->rect.left = (int)a2;
        w->rect.top = (int)a3;
    }
    if (!(a6 & 1)) {
        width = (int)a4;
        height = (int)a5;
    }
    w->rect.right = w->rect.left + width;
    w->rect.bottom = w->rect.top + height;
    if (a6 & 0x40)
        w->visible = true;
    if (a6 & 0x80)
        w->visible = false;
    w->dirty = true;
    if (!(a6 & 1))
        ce_send(a0, 5, 0, (CW)(width & 65535) | (CW)height << 16);
    return 1;
}
CE_API(SetCapture)
{
    CW old = capture;
    capture = a0;
    return old;
}
CE_API(ReleaseCapture)
{
    capture = 0;
    return 1;
}
CE_API(IsWindow)
{
    return ce_window(a0) != NULL;
}
CE_API(EnableWindow)
{
    struct ce_window *w = ce_window(a0);
    if (!w)
        return 0;
    bool old = w->enabled;
    w->enabled = a1 != 0;
    return !old;
}
void ce_destroy_window(CW handle)
{
    struct ce_window *w = ce_window(handle);
    if (!w)
        return;
    ce_send(handle, 2, 0, 0);
    for (unsigned i = 0; i < WINDOWS; i++)
        if (windows[i].handle && windows[i].parent == handle)
            ce_destroy_window(windows[i].handle);
    for (unsigned i = 0; i < ARRAY_SIZE(timers); i++)
        if (timers[i].window == handle)
            timers[i].id = 0;
    if (focus == handle)
        focus = 0;
    if (capture == handle)
        capture = 0;
    free(w->text);
    free(w->tabs);
    ce_list_free(w);
    memset(w, 0, sizeof(*w));
}
CE_API(DestroyWindow)
{
    if (!ce_window(a0))
        return 0;
    ce_destroy_window(a0);
    return 1;
}
CE_API(GetSystemMetrics)
{
    switch (a0) {
    case 0:
    case 16:
        return 240;
    case 1:
    case 17:
        return 320;
    case 2:
    case 3:
        return 13;
    case 4:
        return 20;
    case 5:
    case 6:
    case 7:
    case 8:
        return 1;
    case 15:
        return 20;
    default:
        return 0;
    }
}
CE_API(PtInRect)
{
    struct ce_rect *r = PTR(struct ce_rect, a0);
    return (int)a1 >= r->left && (int)a1 < r->right && (int)a2 >= r->top && (int)a2 < r->bottom;
}
CE_API(InflateRect)
{
    struct ce_rect *r = PTR(struct ce_rect, a0);
    r->left -= (int)a1;
    r->top -= (int)a2;
    r->right += (int)a1;
    r->bottom += (int)a2;
    return 1;
}
CE_API(EqualRect)
{
    return !memcmp(PTR(void, a0), PTR(void, a1), sizeof(struct ce_rect));
}
CE_API(SetTimer)
{
    for (unsigned i = 0; i < ARRAY_SIZE(timers); i++)
        if (!timers[i].id || (timers[i].window == a0 && timers[i].id == a1)) {
            CW id = a1 ? a1 : i + 1;
            timers[i].window = a0;
            timers[i].id = id;
            timers[i].interval = MAX(10u, a2);
            timers[i].deadline = gz_time_ms() + timers[i].interval;
            timers[i].proc = a3;
            return id;
        }
    return 0;
}
CE_API(KillTimer)
{
    for (unsigned i = 0; i < ARRAY_SIZE(timers); i++)
        if (timers[i].id == a1 && timers[i].window == a0) {
            timers[i].id = 0;
            return 1;
        }
    return 0;
}
CE_API(RegisterHotKey)
{
    if (a2) {
        ce_error = 50;
        return 0;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(hotkeys); i++)
        if (!hotkeys[i].window) {
            hotkeys[i].window = a0;
            hotkeys[i].id = a1;
            hotkeys[i].modifiers = a2;
            hotkeys[i].key = a3;
            return 1;
        }
    ce_error = 8;
    return 0;
}
CE_API(UnregisterHotKey)
{
    for (unsigned i = 0; i < ARRAY_SIZE(hotkeys); i++)
        if (hotkeys[i].window == a0 && hotkeys[i].id == a1) {
            hotkeys[i].window = 0;
            return 1;
        }
    return 0;
}
CE_API(GetAsyncKeyState)
{
    return a0 < 256 && key_state[a0] ? 0x8001 : 0;
}
CE_API(IsDialogMessageW)
{
    if (!ce_window(a0))
        return 0;
    struct ce_message *m = PTR(struct ce_message, a1);
    if (m->message != 0x100 || m->wparam != 9)
        return 0;
    unsigned current = handle_index(focus);
    for (unsigned n = 1; n <= WINDOWS; n++) {
        struct ce_window *w = &windows[(current + n) % WINDOWS];
        if (w->handle && w->parent == a0 && w->visible && w->enabled) {
            focus = w->handle;
            return 1;
        }
    }
    return 0;
}
CE_API(SystemParametersInfoW)
{
    if (a0 >= 0xfb && a0 <= 0xfe) {
        const char *name = a0 <= 0xfc ? "BattPowerOff" : "ExtPowerOff";
        const char *key = "HKLM\\SYSTEM\\CurrentControlSet\\Control\\Power";
        if (a0 == 0xfc || a0 == 0xfe) {
            if (!a2) {
                ce_error = 87;
                return 0;
            }
            *PTR(CW, a2) = ce_registry_dword(key, name, 0);
            return 1;
        }
        return ce_registry_set_dword(key, name, a1);
    }
    if (a0 == 0x30 && a2) {
        *PTR(struct ce_rect, a2) =
            (struct ce_rect){0, 0, 240, ce_keyboard_open && !ui.hardware_keyboard ? 320 - UI_KB_H : 320};
        return 1;
    }
    printf("wince: unsupported SystemParametersInfo action %u\n", a0);
    ce_error = 50;
    return 0;
}
CE_API(SipGetInfo)
{
    uint8_t *p = PTR(uint8_t, a0);
    if (!p || ce_u32(p) < 48) {
        ce_error = 87;
        return 0;
    }
    ce_put32(p + 4, ce_keyboard_open ? 1 : 0);
    *PTR(struct ce_rect, a0 + 8) = (struct ce_rect){0, 0, 240, ce_keyboard_open ? 320 - UI_KB_H : 320};
    *PTR(struct ce_rect, a0 + 24) = (struct ce_rect){0, 320 - UI_KB_H, 240, 320};
    ce_put32(p + 40, 0);
    return 1;
}
CE_API(SipShowIM)
{
    ce_keyboard_open = (a0 & 1) != 0;
    ui_keyboard_init(&ce_keyboard);
    for (unsigned i = 0; i < WINDOWS; i++)
        if (windows[i].handle && !windows[i].parent)
            ce_post(windows[i].handle, 0x1a, 224, 0);
    return 1;
}

static const struct ce_export exports[] = {
    CE_EXPORT(RegisterClassW),
    CE_EXPORT(DialogBoxIndirectParamW),
    CE_EXPORT(EndDialog),
    CE_EXPORT(MessageBeep),
    CE_EXPORT(CreateWindowExW),
    CE_EXPORT(DefWindowProcW),
    CE_EXPORT(CallWindowProcW),
    CE_EXPORT(SendMessageW),
    CE_EXPORT(PostMessageW),
    CE_EXPORT(PostQuitMessage),
    CE_EXPORT(GetMessageW),
    CE_EXPORT(TranslateMessage),
    CE_EXPORT(DispatchMessageW),
    CE_EXPORT(ShowWindow),
    CE_EXPORT(UpdateWindow),
    CE_EXPORT(InvalidateRect),
    CE_EXPORT(GetClientRect),
    CE_EXPORT(GetWindowRect),
    CE_EXPORT(MoveWindow),
    CE_EXPORT(ClientToScreen),
    CE_EXPORT(FindWindowW),
    CE_EXPORT(GetWindowLongW),
    CE_EXPORT(SetWindowLongW),
    CE_EXPORT(GetParent),
    CE_EXPORT(GetDlgItem),
    CE_EXPORT(GetWindowTextW),
    CE_EXPORT(GetWindowTextLengthW),
    CE_EXPORT(SetWindowTextW),
    CE_EXPORT(SetDlgItemTextW),
    CE_EXPORT(SetDlgItemInt),
    CE_EXPORT(GetDlgItemInt),
    CE_EXPORT(GetDlgItemTextW),
    CE_EXPORT(SetFocus),
    CE_EXPORT(SetForegroundWindow),
    CE_EXPORT(GetActiveWindow),
    CE_EXPORT(GetFocus),
    CE_EXPORT(IsWindowVisible),
    CE_EXPORT(IsWindowEnabled),
    CE_EXPORT(OffsetRect),
    CE_EXPORT(InitCommonControls),
    CE_EXPORT(InitCommonControlsEx),
    CE_EXPORT(SetRect),
    CE_EXPORT(SetWindowPos),
    CE_EXPORT(SetCapture),
    CE_EXPORT(ReleaseCapture),
    CE_EXPORT(IsWindow),
    CE_EXPORT(EnableWindow),
    CE_EXPORT(DestroyWindow),
    CE_EXPORT(GetSystemMetrics),
    CE_EXPORT(PtInRect),
    CE_EXPORT(InflateRect),
    CE_EXPORT(EqualRect),
    CE_EXPORT(SetTimer),
    CE_EXPORT(KillTimer),
    CE_EXPORT(RegisterHotKey),
    CE_EXPORT(UnregisterHotKey),
    CE_EXPORT(GetAsyncKeyState),
    CE_EXPORT(IsDialogMessageW),
    CE_EXPORT(SystemParametersInfoW),
    CE_EXPORT(SipGetInfo),
    CE_EXPORT(SipShowIM),
};
void *ce_resolve_window(const char *name)
{
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++)
        if (!strcmp(name, exports[i].name))
            return exports[i].address;
    return NULL;
}
