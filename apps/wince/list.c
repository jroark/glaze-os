/* String list/combo controls, with WinCE message numbers and WM_COMMAND
 * notifications. The app owns navigation and actions; controls own selection.
 */
#include "runtime.h"
#define ITEMS 256
struct ce_list {
    struct {
        WCHAR *text;
        CW data;
    } items[ITEMS];
    unsigned count, top;
    int selected, last_tap;
    CW tap_time;
    bool combo;
};
static CW dropdown;
bool ce_list_init(struct ce_window *w)
{
    if (strcasecmp(w->klass, "LISTBOX") && strcasecmp(w->klass, "COMBOBOX"))
        return true;
    w->list = calloc(1, sizeof(*w->list));
    if (!w->list)
        return false;
    w->list->selected = w->list->last_tap = -1;
    w->list->combo = !strcasecmp(w->klass, "COMBOBOX");
    return true;
}
void ce_list_free(struct ce_window *w)
{
    if (dropdown == w->handle)
        dropdown = 0;
    if (!w->list)
        return;
    for (unsigned i = 0; i < w->list->count; i++)
        free(w->list->items[i].text);
    free(w->list);
    w->list = NULL;
}
static void select_item(struct ce_window *w, int index, bool double_click)
{
    struct ce_list *l = w->list;
    if (index < 0 || (unsigned)index >= l->count)
        return;
    bool changed = l->selected != index;
    l->selected = index;
    if (l->combo)
        ce_send(w->handle, 0xc, 0, WORD(l->items[index].text));
    unsigned rows = MAX(1, (w->rect.bottom - w->rect.top) / 18);
    if ((unsigned)index < l->top)
        l->top = index;
    if ((unsigned)index >= l->top + rows)
        l->top = index - rows + 1;
    w->dirty = true;
    if (changed && (l->combo || (w->style & 1)))
        ce_send(w->parent, 0x111, w->id | 1u << 16, w->handle);
    if (double_click && !l->combo && (w->style & 1))
        ce_send(w->parent, 0x111, w->id | 2u << 16, w->handle);
}
bool ce_list_message(struct ce_window *w, CW msg, CW wp, CW lp, CW *result)
{
    struct ce_list *l = w->list;
    if (!l)
        return false;
    *result = 0;
    /* Translate combo operations to their list equivalents. */
    if (l->combo)
        switch (msg) {
        case 0x141: /* CB_LIMITTEXT */
            w->text_limit = wp ? MIN(wp, 65535u) : 65535u;
            if (w->combo_edit) ce_send(w->combo_edit, 0xc5, wp, 0);
            return true;
        case 0x140: /* CB_GETEDITSEL */
            if (w->combo_edit) {
                *result = ce_send(w->combo_edit, 0xb0, wp, lp);
                return true;
            }
            if (wp) *PTR(CW, wp) = w->sel_start;
            if (lp) *PTR(CW, lp) = w->sel_end;
            *result = (w->sel_start & 65535u) | w->sel_end << 16;
            return true;
        case 0x142: /* CB_SETEDITSEL */
            if ((w->style & 3) == 3) { *result = (CW)-1; return true; }
            if (w->combo_edit) {
                ce_send(w->combo_edit, 0xb1, (int16_t)lp, (int16_t)(lp >> 16));
                return true;
            }
            w->sel_start = MIN(lp & 65535u, w->length);
            w->sel_end = MIN(lp >> 16, w->length);
            w->cursor = w->sel_end;
            w->dirty = true;
            return true;
        case 0x143:
            msg = 0x180;
            break; /* ADDSTRING */
        case 0x144:
            msg = 0x182;
            break;
        case 0x146:
            msg = 0x18b;
            break; /* GETCOUNT */
        case 0x147:
            msg = 0x188;
            break;
        case 0x148:
            msg = 0x189;
            break;
        case 0x149:
            msg = 0x18a;
            break;
        case 0x14a:
            msg = 0x181;
            break;
        case 0x14b:
            msg = 0x184;
            break;
        case 0x14c: msg = 0x18f; break; /* FINDSTRING */
        case 0x14d: msg = 0x18c; break; /* SELECTSTRING */
        case 0x158: msg = 0x1a2; break; /* FINDSTRINGEXACT */
        case 0x14e:
            msg = 0x186;
            break;
        case 0x150:
            msg = 0x199;
            break;
        case 0x151:
            msg = 0x19a;
            break;
        case 0x14f:
            dropdown = wp ? w->handle : 0;
            return true;
        }
    switch (msg) {
    case 0x18f:
    case 0x18c:
    case 0x1a2: {
        *result = (CW)-1;
        if (!lp || !l->count) break;
        const WCHAR *needle = PTR(WCHAR, lp);
        unsigned start = wp < l->count ? wp + 1 : 0;
        for (unsigned n = 0; n < l->count; n++) {
            unsigned index = (start + n) % l->count, pos = 0;
            const WCHAR *candidate = l->items[index].text;
            while (needle[pos] && candidate[pos]) {
                unsigned a = needle[pos], b = candidate[pos];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) break;
                pos++;
            }
            if (needle[pos] || (msg == 0x1a2 && candidate[pos])) continue;
            *result = index;
            if (msg == 0x18c) {
                l->selected = (int)index;
                if (l->combo) ce_send(w->handle, 0xc, 0, WORD(candidate));
                w->dirty = true;
            }
            break;
        }
        break;
    }
    case 0x180:
    case 0x181: { /* ADDSTRING / INSERTSTRING */
        if (!lp || l->count == ITEMS || ce_wlen(PTR(WCHAR, lp)) > 4096) {
            *result = (CW)-2;
            break;
        }
        unsigned n = ce_wlen(PTR(WCHAR, lp));
        WCHAR *text = malloc((n + 1) * 2);
        if (!text) {
            *result = (CW)-2;
            break;
        }
        memcpy(text, PTR(void, lp), (n + 1) * 2);
        unsigned index = msg == 0x181 ? MIN(wp, l->count) : l->count;
        unsigned sort = l->combo ? 0x100 : 2;
        if (msg == 0x180 && (w->style & sort)) {
            index = 0;
            while (index < l->count && ce_wcmp(l->items[index].text, text) <= 0)
                index++;
        }
        memmove(l->items + index + 1, l->items + index, (l->count - index) * sizeof(l->items[0]));
        l->items[index].text = text;
        l->items[index].data = 0;
        l->count++;
        if (l->selected >= (int)index)
            l->selected++;
        w->dirty = true;
        *result = index;
        break;
    }
    case 0x182:
        if (wp >= l->count) {
            *result = (CW)-1;
            break;
        }
        free(l->items[wp].text);
        memmove(l->items + wp, l->items + wp + 1, (l->count - wp - 1) * sizeof(l->items[0]));
        l->count--;
        if (l->selected == (int)wp)
            l->selected = -1;
        else if (l->selected > (int)wp)
            l->selected--;
        w->dirty = true;
        *result = l->count;
        break;
    case 0x184:
        for (unsigned i = 0; i < l->count; i++)
            free(l->items[i].text);
        l->count = l->top = 0;
        l->selected = l->last_tap = -1;
        w->dirty = true;
        break;
    case 0x186:
        l->selected = wp < l->count ? (int)wp : -1;
        if (l->combo) {
            static const WCHAR empty[] = {0};
            ce_send(w->handle, 0xc, 0, WORD(l->selected < 0 ? empty : l->items[l->selected].text));
        }
        w->dirty = true;
        *result = (CW)l->selected;
        break;
    case 0x188:
        *result = (CW)l->selected;
        break;
    case 0x189:
    case 0x18a:
        if (wp >= l->count) {
            *result = (CW)-1;
            break;
        }
        *result = ce_wlen(l->items[wp].text);
        if (msg == 0x189 && lp)
            memcpy(PTR(void, lp), l->items[wp].text, (*result + 1) * 2);
        break;
    case 0x18b:
        *result = l->count;
        break;
    case 0x18e:
        *result = l->top;
        break;
    case 0x197:
        if (wp >= l->count) {
            *result = (CW)-1;
            break;
        }
        l->top = wp;
        w->dirty = true;
        break;
    case 0x199:
        *result = wp < l->count ? l->items[wp].data : (CW)-1;
        break;
    case 0x19a:
        if (wp < l->count)
            l->items[wp].data = lp;
        else
            *result = (CW)-1;
        break;
    case 0x194:
    case 0x192:
        break; /* tab stops / horizontal extent */
    case 0x202: {
        int y = (int16_t)(lp >> 16);
        if (l->combo) {
            int x = (int16_t)lp;
            if ((w->style & 3) == 3 || x >= w->rect.right - w->rect.left - 18)
                dropdown = dropdown == w->handle ? 0 : w->handle;
            break;
        }
        if (y < 0)
            break;
        int index = y / 18 + l->top;
        CW now = gz_time_ms();
        bool twice = index == l->last_tap && now - l->tap_time < 500;
        l->last_tap = index;
        l->tap_time = twice ? 0 : now;
        select_item(w, index, twice);
        break;
    }
    case 0x100:
        if (wp == 0x26 || wp == 0x28)
            select_item(w, MAX(0, MIN((int)l->count - 1, l->selected + (wp == 0x28 ? 1 : -1))), false);
        else if (wp == 13)
            select_item(w, l->selected, true);
        else
            return false;
        break;
    default:
        return false;
    }
    return true;
}
static void row(struct ce_window *w, gfx_rect r, unsigned index)
{
    struct ce_list *l = w->list;
    if (index >= l->count)
        return;
    char text[1024];
    ce_narrow(text, sizeof(text), l->items[index].text);
    bool selected = (int)index == l->selected;
    gfx_fill(&ui.surf, r, selected ? 0x305080 : 0xffffff);
    gfx_text(&ui.surf, r.x + 2, r.y + 2, ui_font(FONT_R12), text, selected ? 0xffffff : 0, 255);
}
void ce_list_draw(struct ce_window *w)
{
    gfx_rect r = ce_window_rect(w->handle);
    struct ce_list *l = w->list;
    if (l->combo)
        r.h = 24;
    gfx_set_clip(&ui.surf, r);
    gfx_fill(&ui.surf, r, 0xffffff);
    if (l->combo) {
        if ((w->style & 3) != 3) {
            char text[1024];
            ce_narrow(text, sizeof(text), w->text);
            gfx_text(&ui.surf, r.x + 2, r.y + 3, ui_font(FONT_R12), text, 0, 255);
        } else if (l->selected >= 0)
            row(w, r, l->selected);
        gfx_text(&ui.surf, r.x + r.w - 12, r.y + 3, ui_font(FONT_R12), "v", 0, 255);
    } else
        for (unsigned i = 0; i * 18 < (unsigned)r.h && l->top + i < l->count; i++)
            row(w, gfx_r(r.x, r.y + i * 18, r.w, 18), l->top + i);
    gfx_reset_clip(&ui.surf);
}
static gfx_rect popup(struct ce_window *w)
{
    gfx_rect r = ce_window_rect(w->handle);
    r.y += 24;
    r.h = MIN(240u, w->list->count * 18);
    if (r.y + r.h > 320)
        r.y = MAX(0, r.y - 24 - r.h);
    return r;
}
void ce_list_popup_draw(void)
{
    struct ce_window *w = ce_window(dropdown);
    if (!w || !w->list || !ce_window_visible(dropdown))
        return;
    gfx_rect r = popup(w);
    gfx_set_clip(&ui.surf, r);
    for (unsigned i = 0; i < w->list->count && i * 18 < (unsigned)r.h; i++)
        row(w, gfx_r(r.x, r.y + i * 18, r.w, 18), i);
    gfx_reset_clip(&ui.surf);
}
bool ce_list_popup_input(const struct gz_input_event *e)
{
    struct ce_window *w = ce_window(dropdown);
    if (!w || !w->list || !ce_window_visible(dropdown))
        return false;
    if (e->type != GZ_EV_TOUCH_DOWN && e->type != GZ_EV_TOUCH_UP && e->type != GZ_EV_TOUCH_MOVE)
        return false;
    if (e->type == GZ_EV_TOUCH_UP) {
        gfx_rect r = popup(w);
        dropdown = 0;
        if (gfx_rect_contains(r, e->x, e->y))
            select_item(w, (e->y - r.y) / 18, false);
        ce_redraw_all();
    }
    return true;
}
