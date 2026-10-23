/* Bounded tab items retain application data independently of their labels. */
#include "runtime.h"
CW ce_tabs_message(struct ce_window *w, CW message, CW wp, CW lp, bool *handled)
{
    *handled = w->tabs != NULL;
    if (!*handled) return 0;
    CW handle = w->handle;
    struct ce_tabs *tabs = w->tabs;
    switch (message) {
    case 0x1304: /* TCM_GETITEMCOUNT */
        return tabs->count;
    case 0x133e: { /* TCM_INSERTITEMW */
        if (!lp || tabs->count == ARRAY_SIZE(tabs->text))
            return 0xffffffffu;
        unsigned index = MIN(wp, tabs->count);
        memmove(tabs->text[index + 1], tabs->text[index], (tabs->count - index) * sizeof(tabs->text[0]));
        memmove(tabs->param + index + 1, tabs->param + index, (tabs->count - index) * sizeof(CW));
        memmove(tabs->image + index + 1, tabs->image + index, (tabs->count - index) * sizeof(CW));
        memmove(tabs->state + index + 1, tabs->state + index, (tabs->count - index) * sizeof(CW));
        const CW *item = PTR(CW, lp);
        unsigned n = (item[0] & 1) ? MIN(ce_wlen(PTR(WCHAR, item[3])), 63u) : 0;
        if (n)
            memcpy(tabs->text[index], PTR(void, item[3]), n * 2);
        tabs->text[index][n] = 0;
        tabs->param[index] = item[0] & 8 ? item[6] : 0;
        tabs->image[index] = item[0] & 2 ? item[5] : (CW)-1;
        tabs->state[index] = item[0] & 0x10 ? item[1] & item[2] : 0;
        tabs->count++;
        if (tabs->selected < 0)
            tabs->selected = 0;
        else if ((unsigned)tabs->selected >= index)
            tabs->selected++;
        w->dirty = true;
        return index;
    }
    case 0x133c: /* TCM_GETITEMW */
    case 0x133d: { /* TCM_SETITEMW */
        if (!lp || wp >= tabs->count) return 0;
        CW *item = PTR(CW, lp);
        if (message == 0x133c) {
            if (item[0] & 1) {
                unsigned n = item[4] ? MIN(ce_wlen(tabs->text[wp]), item[4] - 1) : 0;
                if (item[3] && item[4]) {
                    memcpy(PTR(void, item[3]), tabs->text[wp], n * 2);
                    PTR(WCHAR, item[3])[n] = 0;
                }
            }
            if (item[0] & 2) item[5] = tabs->image[wp];
            if (item[0] & 8) item[6] = tabs->param[wp];
            if (item[0] & 0x10) item[1] = tabs->state[wp] & item[2];
        } else {
            if (item[0] & 1) {
                unsigned n = MIN(ce_wlen(PTR(WCHAR, item[3])), 63u);
                if (n) memcpy(tabs->text[wp], PTR(void, item[3]), n * 2);
                tabs->text[wp][n] = 0;
            }
            if (item[0] & 2) tabs->image[wp] = item[5];
            if (item[0] & 8) tabs->param[wp] = item[6];
            if (item[0] & 0x10) tabs->state[wp] = (tabs->state[wp] & ~item[2]) | (item[1] & item[2]);
            w->dirty = true;
        }
        return 1;
    }
    case 0x1308: /* TCM_DELETEITEM */
        if (wp >= tabs->count) return 0;
        tabs->count--;
        memmove(tabs->text[wp], tabs->text[wp + 1], (tabs->count - wp) * sizeof(tabs->text[0]));
        memmove(tabs->param + wp, tabs->param + wp + 1, (tabs->count - wp) * sizeof(CW));
        memmove(tabs->image + wp, tabs->image + wp + 1, (tabs->count - wp) * sizeof(CW));
        memmove(tabs->state + wp, tabs->state + wp + 1, (tabs->count - wp) * sizeof(CW));
        if (tabs->selected == (int)wp) tabs->selected = -1;
        else if (tabs->selected > (int)wp) tabs->selected--;
        w->dirty = true;
        return 1;
    case 0x1309: /* TCM_DELETEALLITEMS */
        tabs->count = 0;
        tabs->selected = -1;
        w->dirty = true;
        return 1;
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
    *handled = false;
    return 0;
}
