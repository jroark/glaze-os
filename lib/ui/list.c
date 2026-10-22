/* libui lists: scrolling rows with press highlighting and tap detection. */

#include <glaze/ui.h>

void ui_list_init(struct ui_list *l, gfx_rect area, int row_h)
{
    l->area = area;
    l->row_h = row_h;
    l->pad = l->footer_h = 0;
    ui_list_reset(l, 0);
}

void ui_list_reset(struct ui_list *l, int count)
{
    l->pressed = -1;
    ui_scroll_reset(&l->sc, 0, l->area.h);
    ui_list_set_count(l, count);
}

void ui_list_set_count(struct ui_list *l, int count)
{
    l->count = count;
    if (l->pressed >= count)
        l->pressed = -1;
    l->sc.content_h = l->pad + count * l->row_h + l->footer_h;
    if (l->sc.offset > ui_scroll_max(&l->sc))
        l->sc.offset = ui_scroll_max(&l->sc);
}

void ui_list_set_area(struct ui_list *l, gfx_rect area)
{
    l->area = area;
    l->sc.view_h = area.h;
    if (l->sc.offset > ui_scroll_max(&l->sc))
        l->sc.offset = ui_scroll_max(&l->sc);
}

int ui_list_row_y(const struct ui_list *l, int i)
{
    return l->area.y + l->pad + i * l->row_h - l->sc.offset;
}

int ui_list_row_at(const struct ui_list *l, int y)
{
    if (y < l->area.y || y >= l->area.y + l->area.h)
        return -1;
    int r = y - l->area.y - l->pad + l->sc.offset;
    if (r < 0)
        return -1;
    r /= l->row_h;
    return r < l->count ? r : -1;
}

int ui_list_event(struct ui_list *l, const struct gz_input_event *e, bool *dirty)
{
    int y = e->y - l->area.y;

    switch (e->type) {
    case GZ_EV_TOUCH_DOWN:
        if (!gfx_rect_contains(l->area, e->x, e->y))
            return -1;
        ui_scroll_event(&l->sc, e, y);
        /* A touch that stops a fling selects nothing. */
        l->pressed = l->sc.moved ? -1 : ui_list_row_at(l, e->y);
        *dirty = true;
        break;
    case GZ_EV_TOUCH_MOVE:
        if (!l->sc.dragging)
            return -1;
        if (ui_scroll_event(&l->sc, e, y))
            *dirty = true;
        if (l->sc.moved && l->pressed >= 0) {
            l->pressed = -1;
            *dirty = true;
        }
        break;
    case GZ_EV_TOUCH_CANCEL:
        if (!l->sc.dragging)
            return -1;
        ui_scroll_event(&l->sc, e, y);
        l->pressed = -1;
        *dirty = true;
        break;
    case GZ_EV_TOUCH_UP: {
        if (!l->sc.dragging)
            return -1;
        ui_scroll_event(&l->sc, e, y);
        int i = l->pressed;
        l->pressed = -1;
        *dirty = true;
        if (i >= 0 && !l->sc.moved && ui_list_row_at(l, e->y) == i)
            return i;
        break;
    }
    }
    return -1;
}

void ui_list_draw(gfx_surface *s, const struct ui_list *l, ui_list_row_fn draw_row, void *ctx)
{
    gfx_rect old = s->clip, clip;

    if (!gfx_rect_intersect(old, l->area, &clip))
        return;
    s->clip = clip;
    for (int i = 0; i < l->count; i++) {
        int y = ui_list_row_y(l, i);
        if (y + l->row_h <= l->area.y)
            continue;
        if (y >= l->area.y + l->area.h)
            break;
        draw_row(s, i, gfx_r(l->area.x, y, l->area.w, l->row_h), i == l->pressed, ctx);
    }
    s->clip = old;
}

void ui_draw_list_row(gfx_surface *s, gfx_rect r, bool selected)
{
    if (selected)
        gfx_vgradient(s, gfx_r(r.x, r.y, r.w, r.h - 1), UI_SELECT_TOP, UI_SELECT_BOTTOM, 255);
    gfx_fill(s, gfx_r(r.x, r.y + r.h - 1, r.w, 1), UI_LIST_SEPARATOR);
}

void ui_draw_delete_badge(gfx_surface *s, int cx, int cy, bool armed)
{
    gfx_circle(s, cx, cy, 8, 0xFFFFFF, 255);
    gfx_circle(s, cx, cy, 7, 0xD8261B, 255);
    if (armed)
        gfx_fill(s, gfx_r(cx - 1, cy - 4, 3, 9), 0xFFFFFF);
    else
        gfx_fill(s, gfx_r(cx - 4, cy - 1, 9, 3), 0xFFFFFF);
}

gfx_rect ui_draw_delete_button(gfx_surface *s, int right, int cy, bool pressed)
{
    gfx_rect r = gfx_r(right - 62, cy - 13, 62, 26);

    gfx_round_rect(s, r, 5, 0x7A0E08, 255);
    gfx_round_rect_vgradient(s, gfx_r(r.x + 1, r.y + 1, r.w - 2, r.h - 2), 4, pressed ? 0xA8231A : 0xEE5A50,
                             pressed ? 0x7A0E08 : 0xC1170D, 255);
    gfx_text_center(s, r.x, r.y + 6, r.w, ui_font(FONT_B12), "Delete", 0xFFFFFF, 255);
    return r;
}
