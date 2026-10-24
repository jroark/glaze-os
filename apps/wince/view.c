/* Tree/list views used by original registry and PIM window procedures.
 * Wire layouts are the 32-bit CE TVITEMW/LVITEMW and NMHDR contracts. */
#include "runtime.h"
#define VIEW_ITEMS 256
#define VIEW_COLS 8
#define TREE_ROOT 0xffff0000u
#define TREE_FIRST 0xffff0001u
#define TREE_LAST 0xffff0002u
#define TREE_SORT 0xffff0003u
struct view_item {
    bool used, expanded;
    CW parent, data, state;
    int children, image;
    WCHAR *text[VIEW_COLS];
};
struct ce_view {
    bool tree;
    unsigned count, top;
    int selected, last_tap;
    CW tap_time, images;
    unsigned order[VIEW_ITEMS];
    struct view_item item[VIEW_ITEMS];
    WCHAR *columns[VIEW_COLS];
    unsigned col_count;
    int widths[VIEW_COLS];
};
static bool copy_text(WCHAR **dst, CW text)
{
    if (text == (CW)-1) {
        ce_error = 50;
        return false;
    }
    unsigned length = ce_wlen(PTR(WCHAR, text));
    if (length > 4096) {
        ce_error = 8;
        return false;
    }
    WCHAR *p = calloc(length + 1, 2);
    if (!p)
        return false;
    if (length)
        memcpy(p, PTR(void, text), length * 2);
    free(*dst);
    *dst = p;
    return true;
}
static void read_text(CW dst, CW capacity, const WCHAR *src)
{
    if (!dst || !capacity)
        return;
    unsigned n = MIN(capacity - 1, ce_wlen(src));
    if (n)
        memcpy(PTR(void, dst), src, n * 2);
    PTR(WCHAR, dst)[n] = 0;
}
static bool owner_text(struct ce_window *w, unsigned row, unsigned column)
{
    struct ce_view *v = w->view;
    WCHAR buffer[512] = {0};
    CW notice[12] = {w->handle, w->id, (CW)-177, 1, row, column,
        0, 0, WORD(buffer), ARRAY_SIZE(buffer), 0, 0};
    ce_send(w->parent, 0x4e, w->id, WORD(notice)); /* LVN_GETDISPINFOW */
    if (w->view != v || row >= v->count)
        return false;
    buffer[ARRAY_SIZE(buffer) - 1] = 0;
    return copy_text(&w->view->item[row].text[column], notice[8]);
}
bool ce_view_init(struct ce_window *w)
{
    bool tree = !strcasecmp(w->klass, "SysTreeView32");
    if (!tree && strcasecmp(w->klass, "SysListView32"))
        return true;
    w->view = calloc(1, sizeof(*w->view));
    if (!w->view)
        return false;
    w->view->tree = tree;
    w->view->selected = w->view->last_tap = -1;
    return true;
}
void ce_view_free(struct ce_window *w)
{
    struct ce_view *v = w->view;
    if (!v)
        return;
    for (unsigned i = 0; i < VIEW_ITEMS; i++)
        for (unsigned c = 0; c < VIEW_COLS; c++)
            free(v->item[i].text[c]);
    for (unsigned c = 0; c < VIEW_COLS; c++)
        free(v->columns[c]);
    free(v);
    w->view = NULL;
}
static int node(struct ce_view *v, CW handle)
{
    unsigned i = handle - 0x79000000u;
    return i < VIEW_ITEMS && v->item[i].used ? (int)i : -1;
}
static CW node_handle(int i) { return i < 0 ? 0 : 0x79000000u + i; }
static void tree_wire(struct ce_view *v, int i, CW out[10])
{
    memset(out, 0, 40);
    if (i < 0)
        return;
    struct view_item *it = &v->item[i];
    out[0] = 0x5f;
    out[1] = node_handle(i);
    out[2] = it->state;
    out[4] = WORD(it->text[0]);
    out[8] = it->children;
    out[9] = it->data;
}
static CW tree_notify(struct ce_window *w, CW code, CW action, int old, int next)
{
    CW n[26] = {w->handle, w->id, code, action};
    tree_wire(w->view, old, n + 4);
    tree_wire(w->view, next, n + 14);
    return ce_send(w->parent, 0x4e, w->id, WORD(n));
}
static int order_pos(struct ce_view *v, int i)
{
    for (unsigned p = 0; p < v->count; p++)
        if (v->order[p] == (unsigned)i)
            return p;
    return -1;
}
static bool list_state(struct ce_window *w, int index, CW state)
{
    struct ce_view *v = w->view;
    CW old = v->item[index].state;
    if (old == state) return true;
    CW n[] = {w->handle, w->id, (CW)-100, (CW)order_pos(v, index), 0,
        state, old, 8, 0, 0, v->item[index].data};
    CW vetoed = (w->style & 0x1000) ? 0 : ce_send(w->parent, 0x4e, w->id, WORD(n));
    if (w->view != v || !v->item[index].used || vetoed) return false;
    v->item[index].state = state;
    if (state & 2) v->selected = index;
    else if (v->selected == index) v->selected = -1;
    w->dirty = true;
    n[2] = (CW)-101;
    ce_send(w->parent, 0x4e, w->id, WORD(n));
    return w->view == v;
}
static void select_row(struct ce_window *w, int index)
{
    struct ce_view *v = w->view;
    if (index < 0 || !v->item[index].used || v->selected == index)
        return;
    int old = v->selected;
    if (!v->tree) {
        if (old >= 0 && !list_state(w, old, v->item[old].state & ~3u)) return;
        list_state(w, index, v->item[index].state | 3);
        return;
    }
    if (v->tree && tree_notify(w, (CW)-450, 0, old, index))
        return;
    if (old >= 0)
        v->item[old].state &= ~3u;
    v->selected = index;
    v->item[index].state |= 3;
    w->dirty = true;
    tree_notify(w, (CW)-451, 0, old, index);
}
static void expand(struct ce_window *w, int i, CW action)
{
    struct ce_view *v = w->view;
    if (i < 0)
        return;
    bool value = action == 2 ? true : action == 1 ? false : !v->item[i].expanded;
    if (value == v->item[i].expanded)
        return;
    if (tree_notify(w, (CW)-454, value ? 2 : 1, -1, i))
        return;
    v->item[i].expanded = value;
    v->item[i].state = (v->item[i].state & ~0x20u) | (value ? 0x20 : 0);
    w->dirty = true;
    tree_notify(w, (CW)-455, value ? 2 : 1, -1, i);
}
static bool visible(struct ce_view *v, unsigned i)
{
    for (unsigned depth = 0; v->item[i].parent && depth < VIEW_ITEMS; depth++) {
        int p = node(v, v->item[i].parent);
        if (p < 0 || !v->item[p].expanded)
            return false;
        i = p;
    }
    return true;
}
static unsigned rows(struct ce_view *v, unsigned out[VIEW_ITEMS])
{
    unsigned count = 0;
    for (unsigned p = 0; p < v->count; p++) {
        unsigned i = v->order[p];
        if (!v->tree || visible(v, i))
            out[count++] = i;
    }
    return count;
}
static void delete_item(struct ce_window *w, unsigned i)
{
    struct ce_view *v = w->view;
    CW h = node_handle(i);
    for (unsigned n = 0; n < VIEW_ITEMS; n++)
        if (v->item[n].used && v->item[n].parent == h)
            delete_item(w, n);
    if (v->tree)
        tree_notify(w, (CW)-458, 0, i, -1);
    for (unsigned c = 0; c < VIEW_COLS; c++)
        free(v->item[i].text[c]);
    memset(&v->item[i], 0, sizeof(v->item[i]));
    int p = order_pos(v, i);
    if (p >= 0) {
        memmove(v->order + p, v->order + p + 1, (v->count - p - 1) * sizeof(v->order[0]));
        v->count--;
    }
    if (v->selected == (int)i)
        v->selected = -1;
    w->dirty = true;
}
bool ce_view_message(struct ce_window *w, CW msg, CW wp, CW lp, CW *result)
{
    struct ce_view *v = w->view;
    if (!v)
        return false;
    *result = 0;
    if (!v->tree && (msg == 7 || msg == 8)) {
        if (v->selected >= 0) {
            int index = v->selected;
            CW state = v->item[index].state;
            list_state(w, index, msg == 7 ? state | 1u : state & ~1u);
        }
        return true;
    }
    if ((v->tree && msg == 0x1109) || (!v->tree && msg == 0x1003)) {
        *result = v->images;
        v->images = lp;
        return true;
    }
    if (v->tree && msg >= 0x1100 && msg < 0x1200) {
        int i = node(v, lp);
        switch (msg) {
        case 0x1132: { /* TVM_INSERTITEMW */
            if (!lp || v->count == VIEW_ITEMS)
                return true;
            const CW *p = PTR(CW, lp), *it = p + 2;
            int parent = p[0] == TREE_ROOT || !p[0] ? -1 : node(v, p[0]);
            if (p[0] && p[0] != TREE_ROOT && parent < 0)
                return true;
            for (i = 0; i < VIEW_ITEMS && v->item[i].used; i++)
                ;
            struct view_item *item = &v->item[i];
            if ((it[0] & 1) && !copy_text(&item->text[0], it[4]))
                return true;
            item->used = true;
            item->parent = node_handle(parent);
            item->data = (it[0] & 4) ? it[9] : 0;
            item->children = (it[0] & 0x40) ? it[8] : 0;
            item->state = (it[0] & 8) ? it[2] & it[3] : 0;
            item->expanded = (item->state & 0x20) != 0;
            unsigned pos = parent < 0 ? 0 : order_pos(v, parent) + 1;
            while (pos < v->count) {
                unsigned n = v->order[pos];
                if (v->item[n].parent == item->parent &&
                    (p[1] == TREE_FIRST || (p[1] == TREE_SORT && item->text[0] && v->item[n].text[0] &&
                                            ce_wcmp(item->text[0], v->item[n].text[0]) < 0)))
                    break;
                if (parent >= 0) {
                    int ancestor = n;
                    while (ancestor >= 0 && ancestor != parent)
                        ancestor = node(v, v->item[ancestor].parent);
                    if (ancestor < 0)
                        break;
                }
                pos++;
                if (node_handle(n) == p[1])
                    break;
            }
            memmove(v->order + pos + 1, v->order + pos, (v->count - pos) * sizeof(unsigned));
            v->order[pos] = i;
            v->count++;
            w->dirty = true;
            *result = node_handle(i);
            return true;
        }
        case 0x1101: /* DELETEITEM */
            if (lp == TREE_ROOT) {
                while (v->count)
                    delete_item(w, v->order[0]);
                *result = 1;
            } else if (i >= 0) {
                delete_item(w, i);
                *result = 1;
            }
            return true;
        case 0x1102:
            expand(w, i, wp);
            *result = i >= 0;
            return true;
        case 0x1105:
            *result = v->count;
            return true;
        case 0x110b:
            if (wp == 9) {
                select_row(w, i);
                *result = i >= 0;
            }
            return true;
        case 0x110a: {
            if (wp == 9) {
                *result = node_handle(v->selected);
                return true;
            }
            CW parent = wp == 0 ? 0 : i < 0 ? (CW)-1 : v->item[i].parent;
            if (wp == 3) {
                *result = parent == (CW)-1 ? 0 : parent;
                return true;
            }
            if (wp == 4)
                parent = node_handle(i);
            int pos = wp == 0 || wp == 4 ? 0 : order_pos(v, i) + (wp == 2 ? -1 : 1), step = wp == 2 ? -1 : 1;
            for (; pos >= 0 && pos < (int)v->count; pos += step) {
                unsigned n = v->order[pos];
                if (v->item[n].parent == parent) {
                    *result = node_handle(n);
                    break;
                }
            }
            return true;
        }
        case 0x113e:
        case 0x113f: { /* GETITEMW / SETITEMW */
            if (!lp)
                return true;
            CW *p = PTR(CW, lp);
            i = node(v, p[1]);
            if (i < 0)
                return true;
            struct view_item *it = &v->item[i];
            if (msg == 0x113e) {
                if (p[0] & 1)
                    read_text(p[4], p[5], it->text[0]);
                if (p[0] & 4)
                    p[9] = it->data;
                if (p[0] & 8)
                    p[2] = it->state & p[3];
                if (p[0] & 0x40)
                    p[8] = it->children;
            } else {
                if ((p[0] & 1) && !copy_text(&it->text[0], p[4]))
                    return true;
                if (p[0] & 4)
                    it->data = p[9];
                if (p[0] & 8)
                    it->state = (it->state & ~p[3]) | (p[2] & p[3]);
                if (p[0] & 0x40)
                    it->children = p[8];
                w->dirty = true;
            }
            *result = 1;
            return true;
        }
        default:
            return false;
        }
    }
    if (!v->tree && msg >= 0x1000 && msg < 0x1100) {
        int i = wp < v->count ? (int)v->order[wp] : -1;
        switch (msg) {
        case 0x102f: /* LVM_SETITEMCOUNT for LVS_OWNERDATA */
            if (!(w->style & 0x1000) || wp > VIEW_ITEMS) {
                ce_error = wp > VIEW_ITEMS ? 8 : 87;
                return true;
            }
            for (unsigned n = wp; n < v->count; n++) {
                for (unsigned c = 0; c < VIEW_COLS; c++) {
                    free(v->item[n].text[c]);
                    v->item[n].text[c] = NULL;
                }
                memset(&v->item[n], 0, sizeof(v->item[n]));
            }
            for (unsigned n = 0; n < wp; n++) {
                v->item[n].used = true;
                v->order[n] = n;
            }
            v->count = wp;
            if (v->selected >= (int)wp)
                v->selected = -1;
            if (v->top >= wp)
                v->top = 0;
            w->dirty = true;
            *result = 1;
            return true;
        case 0x1004:
            *result = v->count;
            return true;
        case 0x104d: { /* INSERTITEMW */
            *result = (CW)-1;
            if (!lp || v->count == VIEW_ITEMS)
                return true;
            const CW *p = PTR(CW, lp);
            unsigned pos = MIN(p[1], v->count);
            for (i = 0; i < VIEW_ITEMS && v->item[i].used; i++)
                ;
            struct view_item *it = &v->item[i];
            if ((p[0] & 1) && !copy_text(&it->text[0], p[5]))
                return true;
            it->used = true;
            it->data = (p[0] & 4) ? p[8] : 0;
            it->image = (p[0] & 2) ? (int)p[7] : -1;
            memmove(v->order + pos + 1, v->order + pos, (v->count - pos) * sizeof(unsigned));
            v->order[pos] = i;
            v->count++;
            w->dirty = true;
            *result = pos;
            if ((p[0] & 8) && (p[3] & 2))
                select_row(w, i);
            return true;
        }
        case 0x1008:
            if (i >= 0) {
                delete_item(w, i);
                *result = 1;
            }
            return true;
        case 0x1009:
            while (v->count)
                delete_item(w, v->order[0]);
            *result = 1;
            return true;
        case 0x104b:
        case 0x104c:
        case 0x1073:
        case 0x1074: {
            if (!lp)
                return true;
            CW *p = PTR(CW, lp);
            if (msg == 0x104b || msg == 0x104c)
                i = p[1] < v->count ? (int)v->order[p[1]] : -1;
            if (i < 0 || p[2] >= VIEW_COLS)
                return true;
            struct view_item *it = &v->item[i];
            bool get = msg == 0x104b || msg == 0x1073;
            if (get && (w->style & 0x1000) && (msg == 0x1073 || (p[0] & 1)) &&
                !owner_text(w, i, p[2]))
                return true;
            if (get) {
                if (msg == 0x1073 || (p[0] & 1))
                    read_text(p[5], p[6], it->text[p[2]]);
                if (msg == 0x104b && (p[0] & 2))
                    p[7] = it->image;
                if (msg == 0x104b && (p[0] & 4))
                    p[8] = it->data;
                if (msg == 0x104b && (p[0] & 8))
                    p[3] = it->state & p[4];
            } else {
                if ((msg == 0x1074 || (p[0] & 1)) && !copy_text(&it->text[p[2]], p[5]))
                    return true;
                if (msg == 0x104c && (p[0] & 2))
                    it->image = (int)p[7];
                if (msg == 0x104c && (p[0] & 4))
                    it->data = p[8];
                if (msg == 0x104c && (p[0] & 8) && (p[3] & 2))
                    select_row(w, i);
                w->dirty = true;
            }
            *result = msg == 0x1073 ? ce_wlen(it->text[p[2]]) : 1;
            return true;
        }
        case 0x1061: { /* INSERTCOLUMNW */
            *result = (CW)-1;
            if (!lp || wp >= VIEW_COLS)
                return true;
            const CW *p = PTR(CW, lp);
            if ((p[0] & 4) && !copy_text(&v->columns[wp], p[3]))
                return true;
            v->widths[wp] = (p[0] & 2) ? p[2] : 100;
            v->col_count = MAX(v->col_count, wp + 1);
            w->dirty = true;
            *result = wp;
            return true;
        }
        case 0x101e:
            if (wp < VIEW_COLS) {
                v->widths[wp] = (int16_t)lp;
                w->dirty = true;
                *result = 1;
            }
            return true;
        case 0x100c:
            *result = (CW)-1;
            for (unsigned p = wp + 1; p < v->count; p++)
                if (!(lp & 3) || v->selected == (int)v->order[p]) {
                    *result = p;
                    break;
                }
            return true;
        case 0x1032:
            *result = v->selected >= 0;
            return true;
        case 0x102c: /* LVM_GETITEMSTATE */
            *result = i >= 0 ? v->item[i].state & lp : 0;
            return true;
        case 0x102b:
            if (lp && (i >= 0 || wp == (CW)-1)) {
                CW *p = PTR(CW, lp);
                for (unsigned n = 0; n < v->count; n++) {
                    int index = (int)v->order[n];
                    if (wp != (CW)-1 && index != i) continue;
                    if ((p[4] & 2) && (p[3] & 2)) {
                        select_row(w, index);
                        if (w->view != v) return true;
                        if (v->selected != index) continue; /* selection was vetoed */
                    }
                    list_state(w, index, (v->item[index].state & ~p[4]) | (p[3] & p[4]));
                    if (w->view != v) return true;
                }
                w->dirty = true;
            }
            *result = 1;
            return true;
        default:
            return false;
        }
    }
    if (msg == 0x202 || msg == 0x100) {
        unsigned order[VIEW_ITEMS], count = rows(v, order);
        int row = (int16_t)(lp >> 16) / 18 + v->top;
        if (!v->tree && v->col_count)
            row--;
        if (msg == 0x100) {
            row = 0;
            while (row < (int)count && (int)order[row] != v->selected)
                row++;
            if (wp == 0x26)
                row--;
            else if (wp == 0x28)
                row++;
            else if (wp != 13)
                return false;
        }
        if (row >= 0 && row < (int)count) {
            int i = order[row];
            CW now = gz_time_ms();
            bool twice = msg == 0x100 ? wp == 13 : (v->last_tap == i && now - v->tap_time < 500);
            v->last_tap = i;
            v->tap_time = twice ? 0 : now;
            select_row(w, i);
            if (w->view != v || !v->item[i].used) return true;
            if (v->tree) {
                if (twice || (int16_t)lp < 24)
                    expand(w, i, 3);
            } else if (msg == 0x202 || twice) {
                /* NM_CLICK/NM_DBLCLK carry a complete NMITEMACTIVATE, including
                 * lParam. A short NMHDR+iItem leaves original handlers reading
                 * unrelated stack words as their persistent record ID. */
                CW n[] = {w->handle, w->id, msg == 0x100 ? (CW)-114 : twice ? (CW)-3 : (CW)-2, (CW)row, 0,
                    0, 0, 0, (CW)(int32_t)(int16_t)lp, (CW)(int32_t)(int16_t)(lp >> 16),
                    v->item[i].data, 0};
                ce_send(w->parent, 0x4e, w->id, WORD(n));
                if (w->view != v) return true;
                if (twice && msg == 0x202) {
                    n[2] = (CW)-114; /* LVN_ITEMACTIVATE follows a double click. */
                    ce_send(w->parent, 0x4e, w->id, WORD(n));
                    if (w->view != v) return true;
                }
            }
            if (row < (int)v->top)
                v->top = row;
            unsigned height = MAX(1, (w->rect.bottom - w->rect.top) / 18 - 1);
            if (row >= (int)(v->top + height))
                v->top = row - height + 1;
        }
        return true;
    }
    return false;
}
void ce_view_draw(struct ce_window *w)
{
    struct ce_view *v = w->view;
    gfx_rect r = ce_window_rect(w->handle);
    gfx_set_clip(&ui.surf, r);
    gfx_fill(&ui.surf, r, 0xffffff);
    int y = r.y;
    if (!v->tree && v->col_count) {
        gfx_fill(&ui.surf, gfx_r(r.x, y, r.w, 18), 0xc0c0c0);
        int x = r.x;
        for (unsigned c = 0; c < v->col_count; c++) {
            char text[256];
            ce_narrow(text, sizeof(text), v->columns[c]);
            gfx_text(&ui.surf, x + 2, y + 2, ui_font(FONT_R12), text, 0, 255);
            x += MAX(24, v->widths[c]);
        }
        y += 18;
    }
    unsigned order[VIEW_ITEMS], count = rows(v, order);
    for (unsigned p = v->top; p < count && y < r.y + r.h; p++, y += 18) {
        unsigned i = order[p];
        struct view_item *it = &v->item[i];
        bool selected = (int)i == v->selected;
        CW color = selected ? 0xffffff : 0;
        gfx_fill(&ui.surf, gfx_r(r.x, y, r.w, 18), selected ? 0x305080 : 0xffffff);
        int x = r.x;
        if (v->tree) {
            int ancestor = node(v, it->parent);
            unsigned depth = 0;
            while (ancestor >= 0 && depth < VIEW_ITEMS) {
                depth++;
                ancestor = node(v, v->item[ancestor].parent);
            }
            x += depth * 12;
            if (it->children)
                gfx_text(&ui.surf, x + 2, y + 2, ui_font(FONT_R12), it->expanded ? "-" : "+", color, 255);
            x += 14;
        }
        for (unsigned c = 0; c < (v->tree ? 1 : MAX(1u, v->col_count)); c++) {
            char text[1024];
            if (!v->tree && (w->style & 0x1000) && !owner_text(w, i, c)) {
                gfx_reset_clip(&ui.surf);
                return;
            }
            ce_narrow(text, sizeof(text), it->text[c]);
            gfx_text(&ui.surf, x + 2, y + 2, ui_font(FONT_R12), text, color, 255);
            x += MAX(24, v->widths[c]);
        }
    }
    gfx_reset_clip(&ui.surf);
}
