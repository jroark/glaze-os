/* WinCE standard modal property sheets. Original pages receive their copied
 * PROPSHEETPAGE on WM_INITDIALOG and PSHNOTIFY on selection/apply/cancel. */
#include "dialog_template.h"
#include "property_state.h"
#include "runtime.h"
struct page {
    uint8_t spec[128];
    CW window;
    bool used;
    WCHAR title[64];
};
_Static_assert(offsetof(struct page, spec) % 4 == 0, "PROPSHEETPAGE must be DWORD aligned");
static struct page pages[CE_PROPERTY_PAGES];
struct sheet {
    struct sheet *next;
    struct ce_property_state state;
    struct page *pages[CE_PROPERTY_PAGES];
    CW window, selector, tabs;
};
static struct sheet *sheets;
static void load_title(WCHAR *out, unsigned capacity, CW module, CW source)
{
    if (source > 65535) {
        unsigned n = MIN(ce_wlen(PTR(WCHAR, source)), capacity - 1);
        memcpy(out, PTR(void, source), n * 2);
        out[n] = 0;
        return;
    }
    uint32_t size;
    const uint8_t *p = ce_resource(module, 6, (source >> 4) + 1, &size);
    unsigned pos = 0;
    for (unsigned i = 0; p && i <= (source & 15); i++) {
        if (!ce_range(pos, 2, size))
            break;
        unsigned n = ce_u16(p + pos);
        pos += 2;
        if (!ce_range(pos, n * 2, size))
            break;
        if (i == (source & 15)) {
            n = MIN(n, capacity - 1);
            for (unsigned j = 0; j < n; j++)
                out[j] = ce_u16(p + pos + 2 * j);
            out[n] = 0;
            return;
        }
        pos += n * 2;
    }
    out[0] = 0;
}
static const void *page_template(struct page *p, uint32_t *size)
{
    CW flags = ce_u32(p->spec + 4), resource = ce_u32(p->spec + 12);
    if (!(flags & 1))
        return ce_resource(ce_u32(p->spec + 8), 5, resource, size);
    *size = ce_image_remaining(resource);
    return *size ? PTR(void, resource) : NULL;
}
CE_API(CreatePropertySheetPageW)
{
    if (!a0) {
        ce_error = 87;
        return 0;
    }
    const uint8_t *spec = PTR(uint8_t, a0);
    unsigned size = ce_u32(spec);
    CW flags = ce_u32(spec + 4);
    if (size < 40 || size > 128 || !ce_u32(spec + 24) || (flags & ~0x3fu)) {
        ce_error = 50;
        return 0;
    }
    unsigned index;
    for (index = 0; index < CE_PROPERTY_PAGES && pages[index].used; index++)
        ;
    if (index == CE_PROPERTY_PAGES) {
        ce_error = 8;
        return 0;
    }
    struct page *page = &pages[index];
    memset(page, 0, sizeof(*page));
    memcpy(page->spec, spec, size);
    uint32_t bytes;
    const void *data = page_template(page, &bytes);
    struct ce_dialog_template *d = malloc(sizeof(*d));
    if (!data || !d || !ce_dialog_decode(d, data, bytes)) {
        free(d);
        ce_error = 1813;
        return 0;
    }
    if (flags & 8)
        load_title(page->title, ARRAY_SIZE(page->title), ce_u32(spec + 8), ce_u32(spec + 20));
    else {
        memcpy(page->title, d->title, MIN(sizeof(page->title) - 2, ce_wlen(d->title) * 2));
    }
    free(d);
    page->used = true;
    return 0x7a000000u + index;
}
CE_API(DestroyPropertySheetPage)
{
    unsigned index = a0 - 0x7a000000u;
    if (index >= CE_PROPERTY_PAGES || !pages[index].used || pages[index].window)
        return 0;
    memset(&pages[index], 0, sizeof(pages[index]));
    return 1;
}
static bool prepare(void *context, unsigned index)
{
    struct sheet *s = context;
    struct page *p = s->pages[index];
    struct ce_dialog_template *d = malloc(sizeof(*d));
    uint32_t bytes;
    const void *data = page_template(p, &bytes);
    if (!d || !data || !ce_dialog_decode(d, data, bytes)) {
        free(d);
        return false;
    }
    static const WCHAR klass[] = {'_', 'c', 'e', '_', 'p', 'a', 'g', 'e', 0};
    struct ce_window *w =
        ce_new_window(klass, d->title, 0x40000000u, 3, 49, 234, 234, s->window, index + 1, 0, 0);
    if (!w) {
        free(d);
        return false;
    }
    p->window = w->handle;
    w->dialog_proc = ce_u32(p->spec + 24);
    w->proc = WORD(ce_dialog_proc);
    w->property_page = true;
    CW first = ce_dialog_controls(w->handle, d, 0);
    free(d);
    if (first == 0xffffffffu) {
        ce_destroy_window(w->handle);
        p->window = 0;
        return false;
    }
    ce_call(w->dialog_proc, w->handle, 0x110, first, WORD(p->spec));
    return true;
}
static int32_t notify(void *context, unsigned index, int code, bool closing)
{
    struct sheet *s = context;
    struct ce_window *w = ce_window(s->pages[index]->window);
    if (!w)
        return 1;
    CW notification[] = {s->window, 0, (CW)code, closing};
    w->dialog_message_result = 0;
    ce_send(w->handle, 0x4e, 0, WORD(notification));
    return w->dialog_message_result;
}
static void show(void *context, unsigned index, bool visible)
{
    struct sheet *s = context;
    struct ce_window *w = ce_window(s->pages[index]->window);
    if (w) {
        w->visible = visible;
        w->dirty = true;
    }
    if (visible) {
        ce_keyboard_open = false;
        ce_send(s->selector, 0x14e, index, 0);
        ce_send(s->tabs, 0x130c, index, 0);
    }
    ce_redraw_all();
}
static CW sheet_proc(CW handle, CW message, CW wp, CW lp)
{
    struct ce_window *w = ce_window(handle);
    struct sheet *s = sheets;
    while (s && s->window != handle)
        s = s->next;
    if (!s)
        return 0;
    if (message == 0xf) {
        gfx_fill(&ui.surf, gfx_r(0, 0, 240, 320), 0xc0c0c0);
        gfx_fill(&ui.surf, gfx_r(0, 0, 240, 24), 0x305080);
        char title[192];
        ce_narrow(title, sizeof(title), w->text);
        gfx_text(&ui.surf, 4, 4, ui_font(FONT_R12), title, 0xffffff, 255);
    } else if (message == 0x111 || message == 0x10) {
        unsigned command = message == 0x10 ? 2 : wp & 65535;
        if (command == 0xff10 && wp >> 16 == 1) {
            CW index = ce_send(s->selector, 0x147, 0, 0);
            if (!ce_property_select(&s->state, index))
                ce_send(s->selector, 0x14e, s->state.active, 0);
        } else if ((command == 1 || command == 0x3021) && ce_property_apply(&s->state, command == 1)) {
            if (command == 1) {
                w->dialog_done = true;
                w->dialog_result = 1;
            }
        } else if (command == 2 && ce_property_cancel(&s->state)) {
            w->dialog_done = true;
            w->dialog_result = 2;
        }
    } else if (message == 0x465) { /* PSM_SETCURSEL */
        unsigned index = wp;
        if (lp)
            for (index = 0; index < s->state.count; index++)
                if (lp - 0x7a000000u < CE_PROPERTY_PAGES && s->pages[index] == &pages[lp - 0x7a000000u])
                    break;
        return ce_property_select(&s->state, index);
    } else if (message == 0x474)
        return s->tabs;
    else if (message == 0x476)
        return s->state.active >= 0 ? s->pages[s->state.active]->window : 0;
    else if (message == 0x46e)
        return ce_property_apply(&s->state, false); /* PSM_APPLY */
    return 0;
}
CE_API(PropertySheetW)
{
    if (!a0 || ce_u32(PTR(void, a0)) < 40) {
        ce_error = 87;
        return (CW)-1;
    }
    const CW *header = PTR(CW, a0);
    unsigned flags = header[1], count = header[6];
    if (!count || count > CE_PROPERTY_PAGES || !header[8] || (flags & ~0x18fu)) {
        ce_error = 50;
        return (CW)-1;
    }
    struct sheet *s = calloc(1, sizeof(*s));
    if (!s)
        return (CW)-1;
    s->state = (struct ce_property_state){count, 0, -1, prepare, notify, show, s};
    s->next = sheets;
    sheets = s;
    CW handles[CE_PROPERTY_PAGES] = {0};
    const uint8_t *spec = PTR(uint8_t, header[8]);
    bool valid = true;
    for (unsigned i = 0; i < count; i++) {
        CW id = (flags & 8) ? ce_CreatePropertySheetPageW(WORD(spec), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
                            : ce_u32(spec + 4 * i);
        if (flags & 8)
            spec += ce_u32(spec);
        unsigned index = id - 0x7a000000u;
        if (index >= CE_PROPERTY_PAGES || !pages[index].used || pages[index].window) {
            valid = false;
            break;
        }
        for (unsigned j = 0; j < i; j++)
            if (handles[j] == id)
                valid = false;
        handles[i] = id;
        s->pages[i] = &pages[index];
    }
    CW result = (CW)-1;
    if (valid) {
        WCHAR caption[128];
        load_title(caption, ARRAY_SIZE(caption), header[3], header[5]);
        static const WCHAR klass[] = {'_', 'c', 'e', '_', 's', 'h', 'e', 'e', 't', 0};
        struct ce_window *w =
            ce_new_window(klass, caption, 0x10000000u, 0, 0, 240, 320, 0, 0, WORD(sheet_proc), 0);
        if (w) {
            s->window = w->handle;
            static const WCHAR combo[] = {'C', 'o', 'm', 'b', 'o', 'B', 'o', 'x', 0},
                               button[] = {'B', 'u', 't', 't', 'o', 'n', 0};
            struct ce_window *selector =
                ce_new_window(combo, NULL, 0x50010003u, 3, 24, 234, 24, w->handle, 0xff10, 0, 0);
            s->selector = selector ? selector->handle : 0;
            /* The visible selector fits long page names on this narrow LCD.
             * Keep a real tab control for applications querying PSM_GETTABCONTROL. */
            static const WCHAR tabclass[] = {'S', 'y', 's', 'T', 'a', 'b', 'C', 'o',
                                             'n', 't', 'r', 'o', 'l', '3', '2', 0};
            struct ce_window *tab =
                ce_new_window(tabclass, NULL, 0x40000000u, 0, 0, 240, 320, w->handle, 0x3020, 0, 0);
            s->tabs = tab ? tab->handle : 0;
            for (unsigned i = 0; i < count; i++) {
                ce_send(s->selector, 0x143, 0, WORD(s->pages[i]->title));
                CW item[] = {1, 0, 0, WORD(s->pages[i]->title), 0, 0, 0};
                ce_send(s->tabs, 0x133e, i, WORD(item));
            }
            static const WCHAR ok[] = {'O', 'K', 0}, cancel[] = {'X', 0},
                               apply[] = {'A', 'p', 'p', 'l', 'y', 0};
            bool controls = selector && tab &&
                            ce_new_window(button, ok, 0x50010001u, 160, 0, 38, 24, w->handle, 1, 0, 0) &&
                            ce_new_window(button, cancel, 0x50010000u, 202, 0, 38, 24, w->handle, 2, 0, 0);
            if (!(flags & 0x80))
                controls = controls && ce_new_window(button, apply, 0x50010000u, 114, 0, 44, 24, w->handle,
                                                     0x3021, 0, 0);
            if ((flags & 0x100) && header[9])
                ce_call(header[9], w->handle, 1, 0, 0);
            if (controls && ce_property_select(&s->state, MIN(header[7], count - 1)))
                result = ce_modal_run(w->handle);
            ce_destroy_window(w->handle);
        }
    }
    for (unsigned i = 0; i < count; i++)
        if (handles[i]) {
            struct page *p = &pages[handles[i] - 0x7a000000u];
            memset(p, 0, sizeof(*p));
        }
    struct sheet **link = &sheets;
    while (*link && *link != s)
        link = &(*link)->next;
    if (*link)
        *link = s->next;
    free(s);
    ce_redraw_all();
    return result;
}
void *ce_resolve_property(const char *name)
{
    static const struct ce_export exports[] = {
        CE_EXPORT(CreatePropertySheetPageW), CE_EXPORT(DestroyPropertySheetPage), CE_EXPORT(PropertySheetW)};
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++)
        if (!strcmp(name, exports[i].name))
            return exports[i].address;
    return NULL;
}
