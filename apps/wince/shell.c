/* WinCE command bars, menus, clipboard and modal dialogs. These are service
 * implementations: application commands still run the original window code. */
#include "runtime.h"
#include <glaze/audio.h>

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
CE_API(Shell_NotifyIcon)
{
    /* Glaze has no CE notification area. Report the optional service as
     * unavailable, allowing callers to keep their ordinary main window. */
    ce_error = 50;
    return 0;
}
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
        if (!w || !ce_window_visible(handle) || !ce_modal_allows(handle) ||
            strcmp(w->klass, "_ce_commandbar"))
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
    if (popup && !ce_modal_allows(popup_owner))
        popup = 0;
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
        if (!w || !ce_window_visible(w->handle) || !ce_modal_allows(w->handle) ||
            strcmp(w->klass, "_ce_commandbar"))
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
                if (menu(child->user)) {
                    ce_send(w->parent, 0x116, child->user, 0);
                    ce_send(w->parent, 0x117, child->user, 0);
                    popup = child->user;
                    popup_owner = w->parent;
                    popup_x = MIN(e->x, 60);
                    popup_y = r.y + r.h;
                    ce_redraw_all();
                } else
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
CE_API(CreatePopupMenu)
{
    struct menu *m = calloc(1, sizeof(*m));
    if (!m)
        return 0;
    CW h = ce_new_object(CE_OBJ_MENU, 0, m, sizeof(*m));
    if (!h)
        free(m);
    return h;
}
CE_API(AppendMenuW)
{
    struct menu *m = menu(a0);
    if (!m || m->count == ARRAY_SIZE(m->item) || (a1 & (4 | 0x100))) {
        ce_error = !m ? 1401 : (a1 & (4 | 0x100)) ? 50 : 8;
        return 0;
    }
    if ((a1 & 16) && !menu(a2)) {
        ce_error = 1401;
        return 0;
    }
    struct menu_item *it = &m->item[m->count++];
    it->flags = a1;
    if (a1 & 16)
        it->submenu = a2;
    else
        it->command = a2;
    if (!(a1 & 0x800) && a3) {
        unsigned n = MIN(ce_wlen(PTR(WCHAR, a3)), ARRAY_SIZE(it->text) - 1);
        memcpy(it->text, PTR(void, a3), n * 2);
    }
    return 1;
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
    struct ce_window *w = ce_new_window(klass, NULL, 0x50000000, 0, 0, 240, 26, a1, a2, 0, 0);
    return w ? w->handle : 0;
}
bool ce_shell_attach_menu(CW parent, CW instance, CW id)
{
    struct ce_window *owner = ce_window(parent);
    CW resource = load_menu(instance, id);
    if (!owner || !resource)
        return false;
    static const WCHAR klass[] = {'_', 'c', 'e', '_', 'c', 'o', 'm', 'm', 'a', 'n', 'd', 'b', 'a', 'r', 0};
    struct ce_window *bar = ce_new_window(klass, NULL, 0x50000000u, 0, 24,
        owner->rect.right - owner->rect.left, 26, parent, 0, 0, 0);
    if (!bar)
        return false;
    bar->nonclient = true;
    bar->user = resource;
    owner->menu = resource;
    owner->client_top = 50;
    return true;
}
CE_API(GetMenu)
{
    struct ce_window *w = ce_window(a0);
    return w ? w->menu : 0;
}
/* CSOBARINFO is passed by value after the three leading arguments: this
 * OEM entry point has thirteen words, one more than the common CE adapters. */
CW ce_CSOBar_Create(CW instance, CW parent, CW id, CW x, CW y, CW width, CW height, CW rows,
                    CW flags, CW button_flags, CW extra, CW reserved, CW reserved2)
{
    static const WCHAR klass[] = {'_', 'c', 'e', '_', 'c', 'o', 'm', 'm', 'a', 'n', 'd', 'b', 'a', 'r', 0};
    (void)instance; (void)flags; (void)button_flags; (void)extra; (void)reserved; (void)reserved2;
    if (!rows || rows > 12)
        return 0;
    struct ce_window *p = ce_window(parent);
    int wpx = width == 0xffffffffu ? (p ? p->rect.right - p->rect.left : 240) : (int)width;
    int hpx = width == 0xffffffffu ? (int)rows * 26 : (int)height;
    struct ce_window *w = ce_new_window(klass, NULL, 0x52000000u, x == 0xffffffffu ? 0 : (int)x,
        y == 0xffffffffu ? 0 : (int)y, wpx, hpx, parent, id, 0, 0);
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
    static const WCHAR spacer[] = {'S', 't', 'a', 't', 'i', 'c', 0};
    static const WCHAR empty[] = {0};
    static const WCHAR ok[] = {'O', 'K', 0};
    static const WCHAR cancel[] = {'C', 'a', 'n', 'c', 'e', 'l', 0};
    for (CW i = 0; i < a2; i++) {
        const uint8_t *p = PTR(uint8_t, a3) + i * 76;
        CW label = ce_u32(p + 20);
        CW id = ce_u32(p), kind = ce_u32(p + 4);
        const WCHAR *caption = label >= 65536 && label < 0x80000000u ? PTR(WCHAR, label) :
            id == 1 ? ok : id == 2 ? cancel : empty;
        char text[192];
        ce_narrow(text, sizeof(text), caption);
        int x = (int)ce_u32(p + 36), y = (int)ce_u32(p + 40);
        int width = (int)ce_u32(p + 44), height = (int)ce_u32(p + 48);
        if (width <= 0)
            width = MAX(26, gfx_text_width(ui_font(FONT_R12), text) + 14);
        if (height <= 0)
            height = 22;
        if (y < 0)
            y = 2;
        if (x < 0) {
            x = 6;
            /* AddButtons can append one item per call. Continue the row
             * after its existing controls instead of overlapping them. */
            for (unsigned j = 0; j < CE_WINDOWS; j++) {
                struct ce_window *child = ce_window(0x70000000u + j);
                if (child && child->parent == bar->handle && child->rect.top == y)
                    x = MAX(x, child->rect.right + 4);
            }
        }
        struct ce_window *button =
            ce_new_window(kind ? klass : spacer, caption, 0x50000000u, x, y, width, height,
                           bar->handle, id, 0, 0);
        if (!button)
            return 0;
        /* This flag is a byte in the OEM record. Calendar leaves the
         * neighboring bytes populated with other button metadata. */
        button->enabled = kind && p[8] == 1;
        CW item = ce_u32(p + 24);
        if (menu(item) || ce_window(item))
            button->user = item; /* borrowed popup menu or embedded control */
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
CE_API(CSOBar_ChangeAllButtons)
{
    if (!ce_CSOBar_DeleteAllButtons(a0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))
        return 0;
    return ce_CSOBar_AddButtons(a0, a1, a2, a3, 0, 0, 0, 0, 0, 0, 0, 0);
}
CE_API(CSOBar_SetButtonState)
{
    unsigned position = 0;
    for (unsigned i = 0; i < CE_WINDOWS; i++) {
        struct ce_window *child = ce_window(0x70000000u + i);
        if (!child || child->parent != a0)
            continue;
        bool match = a1 ? child->id == a2 : position == a2;
        position++;
        if (!match)
            continue;
        if (a3 != 1 || (a4 != 1 && a4 != 2)) {
            ce_error = 50;
            return 0;
        }
        child->enabled = a4 == 1;
        child->dirty = true;
        return 1;
    }
    ce_error = 87;
    return 0;
}
CE_API(CSOBar_ChangeMenu)
{
    if (!ce_window(a0))
        return 0;
    struct menu_item *it = find_item(a1, a2, (a3 & 0x400) != 0, 0);
    if (!it)
        return 0;
    /* The OEM wrapper takes EnableMenuItem flags: zero enables, while
     * MF_GRAYED/MF_DISABLED disable. It is not an enable boolean. */
    it->flags = (it->flags & ~3u) | (a3 & 3);
    return 1;
}
CE_API(CommandBar_Height)
{
    struct ce_window *w = ce_window(a0);
    return w ? w->rect.bottom - w->rect.top : 0;
}
CE_API(CSOBar_GetButtonArea)
{
    struct ce_window *bar = ce_window(a0);
    if (!bar || !a1)
        return 0;
    struct ce_rect *r = PTR(struct ce_rect, a1);
    *r = (struct ce_rect){0, 0, bar->rect.right - bar->rect.left - (bar->style & 1 ? 24 : 0), 26};
    return 1;
}
/* Observed: bar, insert-after, x, y, width, height, SWP_* flags, rows
 * (WorldTALK: 240 wide, 52 high, two rows). Combo boxes move to the second
 * row of a two-row bar. */
CE_API(CSOBar_SetWindowPos)
{
    struct ce_window *bar = ce_window(a0);
    if (!bar)
        return 0;
    int rows = (int)a7 > 0 && (int)a7 <= 3 ? (int)a7 : 1;
    int height = (int)a5 > 0 && (int)a5 <= 120 ? (int)a5 : 26 * rows, row = height / rows;
    if (!(a6 & 2)) /* SWP_NOMOVE */
        bar->rect = (struct ce_rect){(int)a2, (int)a3, (int)a2 + (bar->rect.right - bar->rect.left),
                                     (int)a3 + (bar->rect.bottom - bar->rect.top)};
    if (!(a6 & 1)) /* SWP_NOSIZE */
        bar->rect.right = bar->rect.left + (int)MIN(a4, 240u);
    bar->rect.bottom = bar->rect.top + height;
    for (unsigned i = 0; rows > 1 && i < CE_WINDOWS; i++) {
        struct ce_window *child = ce_window(0x70000000u + i);
        if (child && child->parent == a0 && child->list) {
            int height = child->rect.bottom - child->rect.top;
            child->rect.top = row + 2;
            child->rect.bottom = child->rect.top + height;
        }
    }
    bar->dirty = true;
    return 1;
}
CE_API(CSOBar_GetAdornmentsWidth)
{
    struct ce_window *bar = ce_window(a0);
    return bar && (bar->style & 1) ? 24 : 0;
}
CE_API(CSOBar_GetButtonItem)
{
    unsigned position = 0;
    for (unsigned i = 0; i < CE_WINDOWS; i++) {
        struct ce_window *child = ce_window(0x70000000u + i);
        if (!child || child->parent != a0)
            continue;
        bool match = a1 ? child->id == a2 : position == a2;
        position++;
        if (match)
            return a3 == 1 ? child->handle : a3 == 2 ? child->user : 0;
    }
    return 0;
}
/* CSO.DLL packs the arguments for the bar's message 0xfd5: hInstance, a2,
 * style, position, command id, x, width and drop height. WorldTALK passes
 * WS_VSCROLL | CBS_AUTOHSCROLL | CBS_DROPDOWN as the style and toggles the
 * button by command id 0x640 (a5). */
CE_API(CSOBar_InsertComboBox)
{
    struct ce_window *bar = ce_window(a0);
    if (!bar || !a7 || a7 > 240)
        return 0;
    static const WCHAR combo[] = {'C', 'o', 'm', 'b', 'o', 'B', 'o', 'x', 0};
    int y = bar->rect.bottom - bar->rect.top > 26 ? 28 : 2;
    struct ce_window *w = ce_new_window(combo, NULL, a3 | 0x50000000u, (int)a6, y, (int)a7,
                                       22, a0, a5, 0, 0);
    return w ? w->handle : 0;
}
CE_API(CommandBar_AddBitmap)
{
    struct ce_window *w = ce_window(a0);
    if (!w)
        return (CW)-1;
    uint32_t size = 0;
    unsigned count = a3;
    if (a1 == 0xffffffffu) {
        /* Standard small and view strips contain 15 and 12 images. */
        if (a2 != 0 && a2 != 4)
            return (CW)-1;
        count = a2 == 0 ? 15 : 12;
    } else {
        const uint8_t *b = ce_resource(a1, 2, (uint16_t)a2, &size);
        if (!b || size < 40 || !count || count > 256 || ce_u32(b + 4) / 16 < count)
            return (CW)-1;
    }
    if (count > 256 - w->bar_images)
        return (CW)-1;
    unsigned first = w->bar_images;
    w->bar_images += count;
    return first;
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
    /* HWND, shell request index, output, WCHAR capacity. Glaze launches have
     * no COShell request; a pointer return incorrectly selects PIM picker mode. */
    if (a2 && a3)
        *PTR(WCHAR, a2) = 0;
    return a1 == 0xffffffffu ? 1 : 0;
}
CE_API(CoshExecute)
{
    /* HWND, executable, arguments. The original service forwards the launch
     * to COShell. There is no COShell process in a private Glaze app bundle. */
    ce_error = 2;
    return 0;
}
CE_API(MessageBuzzer)
{
    audio_sound(AUDIO_SND_NOTIFY);
    return 1;
}
CE_API(StopBuzzer)
{
    audio_sound_stop();
    return 1;
}
CE_API(sndPlaySoundW)
{
    if (!a0) { audio_sound_stop(); return 1; }
    if (a1 & 4) { ce_error = 50; return 0; } /* Memory WAV playback is unavailable. */
    if (a1 & 2) { ce_error = 2; return 0; } /* SND_NODEFAULT: no replacement for a missing WAV. */
    /* CE permits the default sound when the requested filename is absent. */
    audio_sound(AUDIO_SND_NOTIFY);
    return 1;
}
CE_API(PlaySoundW)
{
    if (a2 & 0x40000u) { ce_error = 50; return 0; } /* SND_RESOURCE */
    return ce_sndPlaySoundW(a0, a2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
CE_API(CLBAPODisable)
{
    /* The CE power driver is absent. Report its ordinary failure result;
     * these optional calls must not terminate a file-save operation. */
    ce_error = 50;
    return 0;
}
CE_API(CLBAPOEnable)
{
    ce_error = 50;
    return 0;
}
CE_API(RegOpenKeyExW);
CE_API(RegQueryValueExW);
CE_API(RegCloseKey);
CE_API(SendtoCheck)
{
    /* The supplied Sendto.dll checks for a nonempty APATH in the selected
     * launcher's registry key. This is a capability query, not a transfer. */
    static const char *apps[] = {"CASIO Mail", "CASIO Notes", "CASIO Calendar", "CASIO Tasks", "CASIO Contacts"};
    if (a0 < 1 || a0 > ARRAY_SIZE(apps)) return 0;
    char key[96];
    WCHAR wide[96], path[260];
    snprintf(key, sizeof(key), "SOFTWARE\\MENU\\%s", apps[a0 - 1]);
    ce_widen(wide, ARRAY_SIZE(wide), key);
    CW handle = 0, type = 0, size = sizeof(path);
    if (ce_RegOpenKeyExW(0x80000002u, WORD(wide), 0, 0, WORD(&handle), 0, 0, 0, 0, 0, 0, 0)) return 0;
    static const WCHAR name[] = {'A','P','A','T','H',0};
    memset(path, 0, sizeof(path));
    CW status = ce_RegQueryValueExW(handle, WORD(name), 0, WORD(&type), WORD(path), WORD(&size), 0, 0, 0, 0, 0, 0);
    ce_RegCloseKey(handle, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    return !status && type == 1 && size >= 2 && path[0] != 0;
}
CE_API(CGDFlushRegistry)
{ /* RegSetValueExW already synchronizes each update. */
    return 1;
}
CE_API(CGDFlushAlarm)
{
    /* Casio's service exports CE alarm records to its global backup files.
     * Glaze has no CE alarm scheduler. The private catalog is durable already;
     * report unavailable alarm export instead of terminating a PIM save. */
    ce_error = 50;
    return 0;
}
CE_API(CSGetUserDiskName)
{
    /* Matching CS.DLL: suffix, output, capacity in WCHARs, disk selector;
     * returns the combined length, excluding its terminator. Selector 1 is
     * the user's documents: Clock's \Alarm and Internet's \Internet data
     * sit in My Documents on original images. */
    static const WCHAR disk[] = {'\\', 'N', 'a', 'n', 'd', ' ', 'D', 'i', 's', 'k', 0};
    static const WCHAR documents[] = {'\\', 'M', 'y', ' ', 'D', 'o', 'c', 'u', 'm', 'e', 'n', 't', 's', 0};
    const WCHAR *base = a3 == 1 ? documents : disk;
    unsigned prefix = ce_wlen(base), suffix = ce_wlen(PTR(WCHAR, a0));
    unsigned length = prefix + suffix;
    if (suffix > 259 || a2 <= length) {
        ce_error = 122;
        return 0;
    }
    if (a1) {
        WCHAR *out = PTR(WCHAR, a1);
        if (suffix)
            memmove(out + prefix, PTR(void, a0), suffix * 2);
        memcpy(out, base, prefix * 2);
        out[length] = 0;
        /* CS.DLL also creates the resulting directory tree. pimdb relies
         * on that side effect before creating its index and record files. */
        char path[FS_PATH_MAX];
        if (!ce_path(path, out) || fs_mkdirs(path)) {
            ce_error = 5;
            return 0;
        }
    }
    return length;
}
/* Matching CS.DLL: writes the temporary folder to its one argument and
 * creates it; returns nonzero on success. */
CE_API(CSGetTempFolder)
{
    static const WCHAR temp[] = {'\\', 'T', 'e', 'm', 'p', '\\', 0};
    char path[FS_PATH_MAX];
    if (!a0 || !ce_path(path, temp) || fs_mkdirs(path))
        return 0;
    memcpy(PTR(void, a0), temp, sizeof(temp));
    return 1;
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
    printf("wince: message box from %08x: %s: %s\n", WORD(__builtin_return_address(0)), title, message);
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
    CE_EXPORT(Shell_NotifyIcon),
    CE_EXPORT(LoadMenuW),
    CE_EXPORT(GetMenu),
    CE_EXPORT(CreatePopupMenu), CE_EXPORT(AppendMenuW),
    {"CreateMenu", (void *)ce_CreatePopupMenu},
    CE_EXPORT(GetSubMenu),
    CE_EXPORT(DestroyMenu),
    CE_EXPORT(EnableMenuItem),
    CE_EXPORT(CheckMenuItem),
    CE_EXPORT(CommandBar_Create),
    CE_EXPORT(CommandBar_Height),
    CE_EXPORT(CommandBar_AddBitmap),
    CE_EXPORT(CommandBar_InsertMenubar),
    CE_EXPORT(CommandBar_Show),
    CE_EXPORT(CommandBar_GetMenu),
    CE_EXPORT(CommandBar_AddAdornments),
    CE_EXPORT(CSOBar_Create),
    CE_EXPORT(InitCSOButton),
    CE_EXPORT(CSOBar_AddButtons),
    CE_EXPORT(CSOBar_DeleteAllButtons),
    CE_EXPORT(CSOBar_ChangeAllButtons), CE_EXPORT(CSOBar_SetButtonState), CE_EXPORT(CSOBar_ChangeMenu),
    CE_EXPORT(CSOBar_GetButtonArea), CE_EXPORT(CSOBar_GetAdornmentsWidth),
    CE_EXPORT(CSOBar_GetButtonItem), CE_EXPORT(CSOBar_InsertComboBox), CE_EXPORT(CSOBar_SetWindowPos),
    {"CSOBar_Show", (void *)ce_CommandBar_Show},
    {"CSOBar_Height", (void *)ce_CommandBar_Height},
    CE_EXPORT(CSOBar_AddAdornments),
    CE_EXPORT(CSOBar_Destroy),
    CE_EXPORT(CoshGetCommandLine),
    CE_EXPORT(CoshExecute),
    CE_EXPORT(MessageBuzzer),
    CE_EXPORT(StopBuzzer),
    CE_EXPORT(sndPlaySoundW), CE_EXPORT(PlaySoundW),
    CE_EXPORT(CLBAPODisable), CE_EXPORT(CLBAPOEnable),
    CE_EXPORT(CGDFlushRegistry),
    CE_EXPORT(CGDFlushAlarm),
    CE_EXPORT(CSGetUserDiskName),
    CE_EXPORT(CSGetTempFolder),
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
    if (!strcasecmp(dll, "Sendto.dll"))
        return !strcmp(name, "SendtoCheck") ? (void *)ce_SendtoCheck : NULL;
    if (!strcasecmp(dll, "clblib.dll")) {
        if (!strcmp(name, "CLBAPODisable")) return (void *)ce_CLBAPODisable;
        if (!strcmp(name, "CLBAPOEnable")) return (void *)ce_CLBAPOEnable;
        return NULL;
    }
    if (strcasecmp(dll, "coredll.dll") && strcasecmp(dll, "commctrl.dll") && strcasecmp(dll, "CSO.dll") &&
        strcasecmp(dll, "COShellApi.dll") && strcasecmp(dll, "GetDisk.dll") && strcasecmp(dll, "CS.dll"))
        return NULL;
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++)
        if (!strcmp(name, exports[i].name))
            return exports[i].address;
    return NULL;
}
