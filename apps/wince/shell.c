/* WinCE command bars, menus, clipboard and modal dialogs. These are service
 * implementations: application commands still run the original window code. */
#include "runtime.h"

struct menu_item {
    WCHAR text[80];
    CW command, submenu, flags;
};
struct menu {
    unsigned count;
    struct menu_item item[32];
};
static CW popup, popup_owner;
static int popup_x, popup_y;
static CW clipboard;
static bool clipboard_open;
static bool popup_return;
static CW popup_result;
static struct {
    CW handle;
    const void *data;
    uint32_t bytes;
} accelerators[8];

static struct menu *menu(CW handle)
{
    struct ce_object *o = ce_object(handle, CE_OBJ_MENU);
    return o ? o->data : NULL;
}
static void text(char *out, unsigned max, const WCHAR *w)
{
    ce_narrow(out, max, w);
    unsigned n = 0;
    for (unsigned i = 0; out[i]; i++)
        if (out[i] != '&' && out[i] != '\t')
            out[n++] = out[i];
        else if (out[i] == '\t')
            break;
    out[n] = 0;
}
static CW parse_menu(const uint8_t *data, uint32_t size, uint32_t *off, unsigned depth)
{
    if (depth > 5)
        return 0;
    struct menu *m = calloc(1, sizeof(*m));
    if (!m)
        return 0;
    CW handle = ce_new_object(CE_OBJ_MENU, 0, m, sizeof(*m));
    if (!handle) {
        free(m);
        return 0;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(m->item); i++) {
        if (!ce_range(*off, 2, size))
            ce_fatal("Truncated WinCE menu resource");
        struct menu_item *it = &m->item[m->count++];
        it->flags = ce_u16(data + *off);
        *off += 2;
        if (!(it->flags & 16)) {
            if (!ce_range(*off, 2, size))
                ce_fatal("Truncated WinCE menu command");
            it->command = ce_u16(data + *off);
            *off += 2;
        }
        unsigned n = 0;
        bool terminated = false;
        while (ce_range(*off, 2, size)) {
            WCHAR c = ce_u16(data + *off);
            *off += 2;
            if (!c) {
                terminated = true;
                break;
            }
            if (n + 1 < ARRAY_SIZE(it->text))
                it->text[n++] = c;
        }
        if (!terminated)
            ce_fatal("Unterminated WinCE menu label");
        if (it->flags & 16)
            it->submenu = parse_menu(data, size, off, depth + 1);
        if (it->flags & 0x80)
            return handle;
    }
    ce_fatal("WinCE menu has too many entries");
}
static CW load_menu(CW instance, CW id)
{
    uint32_t size;
    const uint8_t *data = ce_resource(instance, 4, (uint16_t)id, &size);
    if (!data || size < 4)
        return 0;
    if (ce_u16(data))
        ce_missing("MENUEX resource");
    uint32_t off = 4 + ce_u16(data + 2);
    return parse_menu(data, size, &off, 0);
}

void ce_shell_draw(void)
{
    for (unsigned i = 0; i < CE_WINDOWS; i++) {
        CW handle = 0x70000000u + i;
        struct ce_window *w = ce_window(handle);
        if (!w || !ce_window_visible(handle) || strcmp(w->klass, "_ce_commandbar"))
            continue;
        gfx_rect r = ce_window_rect(handle);
        gfx_fill(&ui.surf, r, 0xc0c0c0);
        gfx_line(&ui.surf, r.x, r.y + r.h - 1, r.x + r.w - 1, r.y + r.h - 1, 0x808080, 255);
        struct menu *m = menu(w->user);
        int x = r.x + 6;
        if (m)
            for (unsigned j = 0; j < m->count; j++) {
                char s[128];
                text(s, sizeof(s), m->item[j].text);
                x += gfx_text(&ui.surf, x, r.y + 5, ui_font(FONT_R12), s, 0, 255) + 16;
            }
        if (w->style & 1)
            gfx_text(&ui.surf, r.x + r.w - 20, r.y + 4, ui_font(FONT_B14), "x", 0, 255);
        for (unsigned j = 0; j < CE_WINDOWS; j++) {
            struct ce_window *child = ce_window(0x70000000u + j);
            if (child && ce_window_visible(child->handle) && child->parent == handle)
                ce_send(child->handle, 0xf, 0, 0);
        }
    }
    struct menu *m = menu(popup);
    if (!m)
        return;
    gfx_rect r = gfx_r(popup_x, popup_y, 180, (int)m->count * 22);
    gfx_fill(&ui.surf, r, 0xffffff);
    gfx_line(&ui.surf, r.x, r.y, r.x + r.w, r.y, 0x404040, 255);
    for (unsigned i = 0; i < m->count; i++) {
        struct menu_item *it = &m->item[i];
        char s[128];
        text(s, sizeof(s), it->text);
        if (it->flags & 0x800)
            gfx_line(&ui.surf, r.x + 3, r.y + (int)i * 22 + 10, r.x + r.w - 3, r.y + (int)i * 22 + 10,
                     0x808080, 255);
        else
            gfx_text(&ui.surf, r.x + 8, r.y + (int)i * 22 + 4, ui_font(FONT_R12), s,
                     (it->flags & 3) ? 0x888888 : 0, 255);
    }
}

bool ce_shell_input(const struct gz_input_event *e)
{
    if (e->type != GZ_EV_TOUCH_DOWN && e->type != GZ_EV_TOUCH_UP && e->type != GZ_EV_TOUCH_MOVE)
        return false;
    if (popup) {
        if (e->type != GZ_EV_TOUCH_UP)
            return true;
        struct menu *m = menu(popup);
        int row = (e->y - popup_y) / 22;
        if (m && e->y >= popup_y && e->x >= popup_x && e->x < popup_x + 180 && row >= 0 &&
            (unsigned)row < m->count) {
            struct menu_item *it = &m->item[row];
            printf("wince: menu item %u flags %08x\n", it->command, it->flags);
            if (!(it->flags & (3 | 0x800))) {
                if (it->submenu) {
                    popup = it->submenu;
                    popup_y = MIN(298 - (int)menu(popup)->count * 22, popup_y + row * 22);
                    ce_render();
                    return true;
                }
                CW command = it->command, owner = popup_owner;
                popup = 0;
                popup_result = command;
                printf("wince: menu command %u\n", command);
                if (!popup_return)
                    ce_post(owner, 0x111, command, 0);
                ce_redraw_all();
                ce_render();
                return true;
            }
        }
        popup = 0;
        for (unsigned i = 0; i < CE_WINDOWS; i++) {
            struct ce_window *w = ce_window(0x70000000u + i);
            if (w)
                w->dirty = true;
        }
        ce_render();
        return true;
    }
    for (unsigned i = 0; i < CE_WINDOWS; i++) {
        struct ce_window *w = ce_window(0x70000000u + i);
        if (!w || !ce_window_visible(w->handle) || strcmp(w->klass, "_ce_commandbar"))
            continue;
        gfx_rect r = ce_window_rect(w->handle);
        if (!gfx_rect_contains(r, e->x, e->y))
            continue;
        if (e->type != GZ_EV_TOUCH_UP)
            return true;
        for (unsigned j = 0; j < CE_WINDOWS; j++) {
            struct ce_window *child = ce_window(0x70000000u + j);
            if (child && ce_window_visible(child->handle) && child->enabled && child->parent == w->handle &&
                gfx_rect_contains(ce_window_rect(child->handle), e->x, e->y)) {
                ce_post(w->parent, 0x111, child->id, child->handle);
                return true;
            }
        }
        if ((w->style & 1) && e->x >= r.x + r.w - 24) {
            ce_post(w->parent, 0x10, 0, 0);
            return true;
        }
        struct menu *m = menu(w->user);
        int x = r.x + 6;
        if (m)
            for (unsigned j = 0; j < m->count; j++) {
                char s[128];
                text(s, sizeof(s), m->item[j].text);
                int width = gfx_text_width(ui_font(FONT_R12), s) + 16;
                if (e->x >= x && e->x < x + width) {
                    if (m->item[j].submenu) {
                        ce_send(w->parent, 0x116, w->user, 0);
                        ce_send(w->parent, 0x117, m->item[j].submenu, j);
                        popup = m->item[j].submenu;
                        popup_owner = w->parent;
                        popup_x = MIN(x, 60);
                        popup_y = r.y + r.h;
                        ce_render();
                    } else
                        ce_post(w->parent, 0x111, m->item[j].command, 0);
                    return true;
                }
                x += width;
            }
        return true;
    }
    return false;
}

CE_API(LoadMenuW)
{
    return load_menu(a0, a1);
}
CE_API(GetSubMenu)
{
    struct menu *m = menu(a0);
    return m && a1 < m->count ? m->item[a1].submenu : 0;
}
CE_API(DestroyMenu)
{
    struct ce_object *o = ce_object(a0, CE_OBJ_MENU);
    if (!o)
        return 0;
    free(o->data);
    memset(o, 0, sizeof(*o));
    return 1;
}
static struct menu_item *find_item(CW h, CW id, bool position, unsigned depth)
{
    struct menu *m = menu(h);
    if (!m || depth > 5)
        return NULL;
    if (position)
        return id < m->count ? &m->item[id] : NULL;
    for (unsigned i = 0; i < m->count; i++) {
        if (m->item[i].command == id)
            return &m->item[i];
        struct menu_item *item = find_item(m->item[i].submenu, id, false, depth + 1);
        if (item)
            return item;
    }
    return NULL;
}
CE_API(EnableMenuItem)
{
    struct menu_item *it = find_item(a0, a1, (a2 & 0x400) != 0, 0);
    if (!it)
        return 0xffffffffu;
    CW old = it->flags & 3;
    it->flags = (it->flags & ~3u) | (a2 & 3);
    return old;
}
CE_API(CheckMenuItem)
{
    struct menu_item *it = find_item(a0, a1, (a2 & 0x400) != 0, 0);
    if (!it)
        return 0xffffffffu;
    CW old = it->flags & 8;
    it->flags = (it->flags & ~8u) | (a2 & 8);
    return old;
}
CE_API(CommandBar_Create)
{
    static const WCHAR klass[] = {'_', 'c', 'e', '_', 'c', 'o', 'm', 'm', 'a', 'n', 'd', 'b', 'a', 'r', 0};
    struct ce_window *w = ce_new_window(klass, NULL, 0x10000000, 0, 0, 240, 26, a1, a2, 0, 0);
    return w ? w->handle : 0;
}
CE_API(CSOBar_Create)
{
    static const WCHAR klass[] = {'_', 'c', 'e', '_', 'c', 'o', 'm', 'm', 'a', 'n', 'd', 'b', 'a', 'r', 0};
    struct ce_window *w = ce_new_window(klass, NULL, 0x10000000, 0, 0, 240, 26, a1, 0, 0, 0);
    if (w)
        w->user = load_menu(a0, a2);
    return w ? w->handle : 0;
}
CE_API(InitCSOButton)
{
    /* The CSOButton class is provided by the compatibility command bar. */
    return 1;
}
CE_API(CSOBar_AddButtons)
{
    struct ce_window *bar = ce_window(a0);
    if (!bar || !a3 || a2 > 16)
        return 0;
    /* Observed BE-300 CSOBUTTON layout: 76-byte entries; command at 0,
     * geometry at 36..48. The original parent handles WM_COMMAND. */
    static const WCHAR klass[] = {'B', 'u', 't', 't', 'o', 'n', 0};
    static const WCHAR ok[] = {'O', 'K', 0};
    int next_x = 6;
    for (CW i = 0; i < a2; i++) {
        const uint8_t *p = PTR(uint8_t, a3) + i * 76;
        CW label = ce_u32(p + 20);
        const WCHAR *caption = label >= 65536 && label < 0x80000000u ? PTR(WCHAR, label) : ok;
        char text[192];
        ce_narrow(text, sizeof(text), caption);
        int x = (int)ce_u32(p + 36), y = (int)ce_u32(p + 40);
        int width = (int)ce_u32(p + 44), height = (int)ce_u32(p + 48);
        if (width <= 0)
            width = MAX(26, gfx_text_width(ui_font(FONT_R12), text) + 14);
        if (height <= 0)
            height = 22;
        if (x < 0)
            x = next_x;
        if (y < 0)
            y = 2;
        next_x = x + width + 4;
        struct ce_window *button =
            ce_new_window(klass, caption, 0x50000000u, x, y, width, height, bar->handle, ce_u32(p), 0, 0);
        if (!button)
            return 0;
    }
    return 1;
}
CE_API(CSOBar_DeleteAllButtons)
{
    struct ce_window *bar = ce_window(a0);
    if (!bar)
        return 0;
    for (unsigned i = 0; i < CE_WINDOWS; i++) {
        struct ce_window *child = ce_window(0x70000000u + i);
        if (child && child->parent == bar->handle)
            ce_destroy_window(child->handle);
    }
    bar->dirty = true;
    return 1;
}
CE_API(CommandBar_Height)
{
    return ce_window(a0) ? 26 : 0;
}
CE_API(CommandBar_InsertMenubar)
{
    struct ce_window *w = ce_window(a0);
    if (!w)
        return 0;
    w->user = load_menu(a1, a2);
    w->dirty = true;
    return w->user != 0;
}
CE_API(CommandBar_Show)
{
    struct ce_window *w = ce_window(a0);
    if (!w)
        return 0;
    w->visible = a1 != 0;
    w->dirty = true;
    return 1;
}
CE_API(CommandBar_GetMenu)
{
    struct ce_window *w = ce_window(a0);
    return w ? w->user : 0;
}
CE_API(CommandBar_AddAdornments)
{
    struct ce_window *w = ce_window(a0);
    if (!w)
        return 0;
    w->style |= 1;
    w->dirty = true;
    return 1;
}
CE_API(CSOBar_AddAdornments)
{
    struct ce_window *w = ce_window(a0);
    if (!w)
        return 0;
    w->style |= a2 ? 1u : 0;
    w->dirty = true;
    return 1;
}
CE_API(CSOBar_Destroy)
{
    if (!ce_window(a0))
        return 0;
    ce_destroy_window(a0);
    ce_redraw_all();
    return 1;
}
CE_API(CoshGetCommandLine)
{
    static WCHAR empty[1];
    return WORD(empty);
}
CE_API(CGDFlushRegistry)
{ /* RegSetValueExW already synchronizes each update. */
    return 1;
}
CE_API(CSGetUserDiskName)
{
    /* Matching CS.DLL: suffix, output, capacity in WCHARs, disk selector;
     * returns the combined length, excluding its terminator. */
    static const WCHAR disk[] = {'\\', 'N', 'a', 'n', 'd', ' ', 'D', 'i', 's', 'k', 0};
    unsigned suffix = ce_wlen(PTR(WCHAR, a0));
    unsigned length = ARRAY_SIZE(disk) - 1 + suffix;
    if (suffix > 259 || a2 <= length) {
        ce_error = 122;
        return 0;
    }
    if (a1) {
        WCHAR *out = PTR(WCHAR, a1);
        if (suffix)
            memmove(out + ARRAY_SIZE(disk) - 1, PTR(void, a0), suffix * 2);
        memcpy(out, disk, sizeof(disk) - 2);
        out[length] = 0;
    }
    return length;
}
CE_API(OpenClipboard)
{
    if (clipboard_open) {
        ce_error = 5;
        return 0;
    }
    clipboard_open = true;
    return 1;
}
CE_API(EmptyClipboard)
{
    if (!clipboard_open)
        return 0;
    free(PTR(void, clipboard));
    clipboard = 0;
    return 1;
}
CE_API(CloseClipboard)
{
    if (!clipboard_open)
        return 0;
    clipboard_open = false;
    return 1;
}
CE_API(SetClipboardData)
{
    if (!clipboard_open)
        return 0;
    if (a0 != 13) {
        ce_error = 50;
        return 0;
    }
    free(PTR(void, clipboard));
    clipboard = a1;
    return a1;
}
CE_API(GetClipboardData)
{
    return clipboard_open && a0 == 13 ? clipboard : 0;
}
CE_API(LoadAcceleratorsW)
{
    uint32_t size;
    const void *p = ce_resource(a0, 9, (uint16_t)a1, &size);
    if (!p)
        return 0;
    for (unsigned i = 0; i < ARRAY_SIZE(accelerators); i++)
        if (!accelerators[i].handle) {
            accelerators[i].handle = 0x77000000u + i;
            accelerators[i].data = p;
            accelerators[i].bytes = size;
            return accelerators[i].handle;
        }
    return 0;
}
CE_API(TranslateAcceleratorW)
{
    unsigned index = a1 - 0x77000000u;
    if (index >= ARRAY_SIZE(accelerators))
        return 0;
    struct ce_message *m = PTR(struct ce_message, a2);
    if (m->message != 0x100)
        return 0;
    const uint8_t *p = accelerators[index].data;
    for (unsigned off = 0; ce_range(off, 8, accelerators[index].bytes); off += 8) {
        CW flags = ce_u16(p + off);
        if (!(flags & 0x1c) && ce_u16(p + off + 2) == m->wparam) {
            ce_send(a0, 0x111, ce_u16(p + off + 4) | 0x10000u, 0);
            return 1;
        }
        if (flags & 0x80)
            break;
    }
    return 0;
}
CE_API(TrackPopupMenuEx)
{
    struct menu *m = menu(a0);
    if (!m)
        return 0;
    popup = a0;
    popup_owner = a4;
    popup_x = MIN(60, MAX(0, (int)a2));
    popup_y = MIN(MAX(0, (int)a3), 320 - (int)m->count * 22);
    popup_return = (a1 & 0x100) != 0;
    popup_result = 0;
    ce_render();
    while (popup) {
        struct gz_input_event e;
        if (ui_next_event(&e, 50))
            ce_shell_input(&e);
    }
    bool direct = popup_return;
    popup_return = false;
    return direct ? popup_result : popup_result != 0;
}

static void repaint(void)
{
    for (unsigned i = 0; i < CE_WINDOWS; i++) {
        struct ce_window *w = ce_window(0x70000000u + i);
        if (w)
            w->dirty = true;
    }
    ce_render();
}

CE_API(MessageBoxW)
{
    char title[128], message[1024];
    ce_narrow(title, sizeof(title), PTR(WCHAR, a2));
    ce_narrow(message, sizeof(message), PTR(WCHAR, a1));
    unsigned style = a3 & 15;
    const char *left = "OK", *right = NULL, *third = NULL;
    CW left_id = 1, right_id = 2, third_id = 2, escape_id = 0;
    if (style == 3 || style == 4) {
        left = "Yes";
        right = "No";
        left_id = 6;
        right_id = 7;
        third = style == 3 ? "Cancel" : NULL;
        escape_id = style == 3 ? 2 : 0;
    } else if (style == 1) {
        right = "Cancel";
        escape_id = 2;
    } else if (style == 2) {
        left = "Retry";
        right = "Ignore";
        third = "Abort";
        left_id = 4;
        right_id = 5;
        third_id = 3;
    } else if (style == 5) {
        left = "Retry";
        right = "Cancel";
        left_id = 4;
        escape_id = 2;
    } else
        escape_id = 1;
    gfx_fill(&ui.surf, gfx_r(0, 0, 240, 320), 0xc0c0c0);
    gfx_fill(&ui.surf, gfx_r(0, 0, 240, 24), 0x000080);
    gfx_text(&ui.surf, 6, 4, ui_font(FONT_B12), title, 0xffffff, 255);
    for (unsigned pos = 0, y = 42; message[pos] && y < 218; y += 16) {
        char line[35];
        unsigned n = 0;
        while (message[pos] && message[pos] != '\n' && n < 34) {
            if (message[pos] != '\r')
                line[n++] = message[pos];
            pos++;
        }
        if (message[pos] == '\n')
            pos++;
        line[n] = 0;
        gfx_text(&ui.surf, 8, (int)y, ui_font(FONT_R12), line, 0, 255);
    }
    gfx_rect l = gfx_r(12, 240, right ? 100 : 216, 30), r = gfx_r(128, 240, 100, 30),
             c = gfx_r(12, 280, 216, 28);
    ui_draw_button(&ui.surf, l, left, false);
    if (right)
        ui_draw_button(&ui.surf, r, right, false);
    if (third)
        ui_draw_button(&ui.surf, c, third, false);
    ui_invalidate_all();
    ui_present();
    CW result = 0;
    while (!result) {
        struct gz_input_event e;
        if (!ui_next_event(&e, 100))
            continue;
        if (e.type == GZ_EV_TOUCH_UP) {
            if (gfx_rect_contains(l, e.x, e.y))
                result = left_id;
            if (right && gfx_rect_contains(r, e.x, e.y))
                result = right_id;
            if (third && gfx_rect_contains(c, e.x, e.y))
                result = third_id;
        }
        if (e.type == GZ_EV_KEY_DOWN && (e.code == GZ_KEY_ENTER || e.code == GZ_KEY_OK))
            result = left_id;
        if (e.type == GZ_EV_KEY_DOWN && e.code == GZ_KEY_ESC)
            result = escape_id;
    }
    repaint();
    return result;
}

static CW file_dialog(CW address, bool save)
{
    uint8_t *ofn = PTR(uint8_t, address);
    printf("wince: %s dialog, struct %u\n", save ? "save" : "open", ce_u32(ofn));
    if (ce_u32(ofn) < 60) {
        ce_error = 87;
        return 0;
    }
    CW buffer = ce_u32(ofn + 28), capacity = ce_u32(ofn + 32);
    if (!buffer || capacity < 2) {
        ce_error = 87;
        return 0;
    }
    char full[FS_PATH_MAX], name[64];
    ce_narrow(full, sizeof(full), PTR(WCHAR, buffer));
    char *last = strrchr(full, '\\');
    if (!last)
        last = strrchr(full, '/');
    strlcpy(name, last ? last + 1 : full, sizeof(name));
    if (!name[0] && save)
        strlcpy(name, "Untitled.txt", sizeof(name));
    struct fs_dirent entries[32];
    int count = fs_list("/Documents", 0, entries, ARRAY_SIZE(entries));
    if (count < 0)
        count = 0;
    struct ui_keyboard kb;
    ui_keyboard_init(&kb);
    bool replace = true, active = false;
    CW result = 0;
    bool done = false;
    while (!done) {
        gfx_fill(&ui.surf, gfx_r(0, 0, 240, 320), 0xc0c0c0);
        gfx_fill(&ui.surf, gfx_r(0, 0, 240, 24), 0x000080);
        gfx_text(&ui.surf, 6, 4, ui_font(FONT_B12), save ? "Save As" : "Open", 0xffffff, 255);
        gfx_text(&ui.surf, 8, 27, ui_font(FONT_R10), "Documents", 0, 255);
        gfx_fill(&ui.surf, gfx_r(6, 43, 228, 25), 0xffffff);
        gfx_text(&ui.surf, 10, 48, ui_font(FONT_R12), name, 0, 255);
        for (int i = 0; i < MIN(count, 3); i++)
            gfx_text(&ui.surf, 10, 76 + i * 19, ui_font(FONT_R11), entries[i].name, 0, 255);
        gfx_rect ok = gfx_r(124, 136, 110, 28), cancel = gfx_r(6, 136, 110, 28);
        ui_draw_button(&ui.surf, ok, save ? "Save" : "Open", false);
        ui_draw_button(&ui.surf, cancel, "Cancel", false);
        if (!ui.hardware_keyboard)
            ui_draw_keyboard(&ui.surf, &kb, 320 - UI_KB_H);
        ui_invalidate_all();
        ui_present();
        struct gz_input_event e;
        if (!ui_next_event(&e, 100))
            continue;
        int ch = e.type == GZ_EV_TEXT ? e.code : 0;
        if (e.type == GZ_EV_KEY_DOWN) {
            if (e.code == GZ_KEY_BACKSPACE)
                ch = 8;
            if (e.code == GZ_KEY_ESC)
                done = true;
            if (e.code == GZ_KEY_ENTER || e.code == GZ_KEY_OK)
                e = (struct gz_input_event){.type = GZ_EV_TOUCH_UP, .x = 180, .y = 150};
        }
        if (!ui.hardware_keyboard && e.y >= 320 - UI_KB_H &&
            (e.type == GZ_EV_TOUCH_DOWN || e.type == GZ_EV_TOUCH_UP || e.type == GZ_EV_TOUCH_MOVE)) {
            bool changed = false;
            ch = ui_keyboard_event(&kb, &e, 320 - UI_KB_H, &changed);
        }
        if (ch && (active || ui.hardware_keyboard)) {
            unsigned n = strlen(name);
            if (replace) {
                n = 0;
                name[0] = 0;
                replace = false;
            }
            if (ch == 8) {
                if (n)
                    name[n - 1] = 0;
            } else if (ch >= 32 && ch < 127 && ch != '/' && ch != '\\' && n + 1 < sizeof(name)) {
                name[n] = (char)ch;
                name[n + 1] = 0;
            }
        }
        if (e.type == GZ_EV_TOUCH_UP) {
            if (e.y >= 43 && e.y < 68) {
                active = true;
                replace = true;
            }
            if (e.y >= 76 && e.y < 133) {
                int i = (e.y - 76) / 19;
                if (i < count && entries[i].type == FS_FILE) {
                    strlcpy(name, entries[i].name, sizeof(name));
                    replace = true;
                }
            }
            if (gfx_rect_contains(cancel, e.x, e.y))
                done = true;
            if (gfx_rect_contains(ok, e.x, e.y) && name[0]) {
                /* lpstrDefExt (OPENFILENAME+60) completes a bare save name. */
                CW default_ext = ce_u32(ofn) >= 64 ? ce_u32(ofn + 60) : 0;
                if (save && default_ext && !strchr(name, '.')) {
                    char ext[16];
                    ce_narrow(ext, sizeof(ext), PTR(WCHAR, default_ext));
                    if (ext[0] && strlen(name) + strlen(ext) + 1 < sizeof(name)) {
                        strlcat(name, ".", sizeof(name));
                        strlcat(name, ext, sizeof(name));
                    }
                }
                snprintf(full, sizeof(full), "/Documents/%s", name);
                struct fs_stat st;
                if (save || (!fs_stat(full, &st) && st.type == FS_FILE)) {
                    char ce_name[FS_PATH_MAX];
                    snprintf(ce_name, sizeof(ce_name), "\\Documents\\%s", name);
                    if (strlen(ce_name) + 1 <= capacity) {
                        ce_widen(PTR(WCHAR, buffer), capacity, ce_name);
                        ofn[56] = 11;
                        ofn[57] = 0;
                        char *dot = strrchr(ce_name, '.');
                        uint16_t extension = dot ? (uint16_t)(dot - ce_name + 1) : 0;
                        ofn[58] = extension & 255;
                        ofn[59] = extension >> 8;
                        CW title = ce_u32(ofn + 36), title_capacity = ce_u32(ofn + 40);
                        if (title && title_capacity > strlen(name))
                            ce_widen(PTR(WCHAR, title), title_capacity, name);
                        result = 1;
                        done = true;
                    } else {
                        ce_error = 122;
                        done = true;
                    }
                }
            }
        }
    }
    repaint();
    printf("wince: %s dialog returned %u\n", save ? "save" : "open", result);
    return result;
}
CE_API(GetOpenFileNameW)
{
    return file_dialog(a0, false);
}
CE_API(GetSaveFileNameW)
{
    return file_dialog(a0, true);
}
CE_API(SHAddToRecentDocs)
{
    if (a0 == 3 && a1) {
        char path[FS_PATH_MAX], folder[FS_PATH_MAX];
        snprintf(folder, sizeof(folder), "%s/data", res_bundle());
        if (!fs_mkdirs(folder)) {
            snprintf(path, sizeof(path), "%s/recent", folder);
            fs_write_file(path, PTR(void, a1), (ce_wlen(PTR(WCHAR, a1)) + 1) * 2);
        }
    }
    return 0;
}
CE_API(SHGetFileInfo)
{
    if (!a2 || a3 < 12)
        return 0;
    memset(PTR(void, a2), 0, a3);
    if (a4 & 0x200) {
        char path[FS_PATH_MAX];
        ce_narrow(path, sizeof(path), PTR(WCHAR, a0));
        char *name = strrchr(path, '\\');
        if (!name)
            name = strrchr(path, '/');
        ce_widen(PTR(WCHAR, a2 + 12), (a3 - 12) / 2, name ? name + 1 : path);
    }
    return (a4 & 0x100) ? 0 : 1;
}

static const struct ce_export exports[] = {
    CE_EXPORT(LoadMenuW),
    CE_EXPORT(GetSubMenu),
    CE_EXPORT(DestroyMenu),
    CE_EXPORT(EnableMenuItem),
    CE_EXPORT(CheckMenuItem),
    CE_EXPORT(CommandBar_Create),
    CE_EXPORT(CommandBar_Height),
    CE_EXPORT(CommandBar_InsertMenubar),
    CE_EXPORT(CommandBar_Show),
    CE_EXPORT(CommandBar_GetMenu),
    CE_EXPORT(CommandBar_AddAdornments),
    CE_EXPORT(CSOBar_Create),
    CE_EXPORT(InitCSOButton),
    CE_EXPORT(CSOBar_AddButtons),
    CE_EXPORT(CSOBar_DeleteAllButtons),
    {"CSOBar_Height", (void *)ce_CommandBar_Height},
    CE_EXPORT(CSOBar_AddAdornments),
    CE_EXPORT(CSOBar_Destroy),
    CE_EXPORT(CoshGetCommandLine),
    CE_EXPORT(CGDFlushRegistry),
    CE_EXPORT(CSGetUserDiskName),
    CE_EXPORT(OpenClipboard),
    CE_EXPORT(EmptyClipboard),
    CE_EXPORT(CloseClipboard),
    CE_EXPORT(SetClipboardData),
    CE_EXPORT(GetClipboardData),
    CE_EXPORT(LoadAcceleratorsW),
    CE_EXPORT(TranslateAcceleratorW),
    CE_EXPORT(TrackPopupMenuEx),
    CE_EXPORT(MessageBoxW),
    CE_EXPORT(GetOpenFileNameW),
    CE_EXPORT(GetSaveFileNameW),
    CE_EXPORT(SHAddToRecentDocs),
    CE_EXPORT(SHGetFileInfo),
};
void *ce_resolve_shell(const char *dll, const char *name)
{
    if (strcasecmp(dll, "coredll.dll") && strcasecmp(dll, "commctrl.dll") && strcasecmp(dll, "CSO.dll") &&
        strcasecmp(dll, "COShellApi.dll") && strcasecmp(dll, "GetDisk.dll") && strcasecmp(dll, "CS.dll"))
        return NULL;
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++)
        if (!strcmp(name, exports[i].name))
            return exports[i].address;
    return NULL;
}
