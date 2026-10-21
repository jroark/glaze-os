/* A WinCE message desktop presented as one SpringBoard surface. Original
 * application window procedures execute directly; built-in controls live here. */
#include "runtime.h"

#define WINDOWS 48
#define QUEUE 128
static struct ce_window windows[WINDOWS];
static struct {
    char name[64];
    CW proc;
} classes[24];
static unsigned class_count;
static struct ce_message queue[QUEUE];
static unsigned head, tail;
static CW focus, capture;
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
    return w ? ce_call(w->proc, handle, message, wparam, lparam) : 0;
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
    w->enabled = true;
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
    if (!set_text(w, text)) {
        w->handle = 0;
        return NULL;
    }
    w->visible = (style & 0x10000000u) != 0;
    CW create[] = {arg,   ce_main.base, id,    parent,     (CW)height,  (CW)width,
                   (CW)y, (CW)x,        style, WORD(text), WORD(klass), 0};
    /* The callback can recursively create child windows. */
    if (ce_send(w->handle, 1, 0, WORD(create)) == 0xffffffffu) {
        free(w->text);
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
    if (length > 65535)
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

static CW builtin_proc(CW handle, CW message, CW wp, CW lp)
{
    struct ce_window *w = ce_window(handle);
    if (!w)
        return 0;
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
        if (strcasecmp(w->klass, "Edit"))
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
    if (!strcasecmp(w->klass, "Edit")) {
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
    } else if (!strcasecmp(w->klass, "Button")) {
        char text[128];
        ce_narrow(text, sizeof(text), w->text);
        gfx_fill(&ui.surf, r, 0xc0c0c0);
        gfx_line(&ui.surf, r.x, r.y, r.x + r.w - 1, r.y, 0x808080, 255);
        gfx_text_center(&ui.surf, r.x, r.y + 3, r.w, ui_font(FONT_R12), text, 0, 255);
    } else if (!strcasecmp(w->klass, "Static")) {
        char text[256];
        ce_narrow(text, sizeof(text), w->text);
        gfx_text(&ui.surf, r.x, r.y, ui_font(FONT_R12), text, 0, 255);
    }
}

void ce_render(void)
{
    for (unsigned i = 0; i < WINDOWS; i++) {
        struct ce_window *w = &windows[i];
        if (!w->handle || !w->visible || !w->dirty)
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
    ui_invalidate_all();
    ui_present();
}

static void input(const struct gz_input_event *e)
{
    if (ce_shell_input(e))
        return;
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
            if (windows[i].handle && windows[i].visible && windows[i].enabled &&
                gfx_rect_contains(ce_window_rect(windows[i].handle), e->x, e->y))
                w = &windows[i];
    if (!w)
        return;
    gfx_rect r = ce_window_rect(w->handle);
    if (e->type == GZ_EV_TOUCH_DOWN) {
        focus = w->handle;
        if (!strcasecmp(w->klass, "Edit")) {
            ce_keyboard_open = true;
            w->dirty = true;
        }
    }
    CW msg = e->type == GZ_EV_TOUCH_DOWN ? 0x201 : e->type == GZ_EV_TOUCH_UP ? 0x202 : 0x200;
    ce_post(w->handle, msg, e->type == GZ_EV_TOUCH_UP ? 0 : 1,
            (uint16_t)(e->x - r.x) | (CW)(uint16_t)(e->y - r.y) << 16);
    if (e->type == GZ_EV_TOUCH_UP && !strcasecmp(w->klass, "Button"))
        ce_post(w->parent, 0x111, w->id, w->handle);
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
CE_API(ShowWindow)
{
    struct ce_window *w = ce_window(a0);
    if (!w)
        return 0;
    bool was = w->visible;
    w->visible = a1 != 0;
    w->dirty = true;
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
    case -21:
    case 0:
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
    case -21:
    case 0:
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
CE_API(DestroyWindow)
{
    struct ce_window *w = ce_window(a0);
    if (!w)
        return 0;
    ce_send(a0, 2, 0, 0);
    free(w->text);
    memset(w, 0, sizeof(*w));
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
    CE_EXPORT(GetDlgItemTextW),
    CE_EXPORT(SetFocus),
    CE_EXPORT(SetForegroundWindow),
    CE_EXPORT(GetActiveWindow),
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
