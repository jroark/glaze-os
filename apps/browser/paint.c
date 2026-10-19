/* Painting the display list into a viewport. */

#include "web.h"

#include <string.h>

#define CTL_BORDER  0x8C8C8C
#define CTL_FOCUS   0x3B7DDD

static void frame(gfx_surface *s, gfx_rect r, uint32_t c, int w)
{
    gfx_fill(s, gfx_r(r.x, r.y, r.w, w), c);
    gfx_fill(s, gfx_r(r.x, r.y + r.h - w, r.w, w), c);
    gfx_fill(s, gfx_r(r.x, r.y, w, r.h), c);
    gfx_fill(s, gfx_r(r.x + r.w - w, r.y, w, r.h), c);
}

static void button_face(gfx_surface *s, gfx_rect r, bool pressed)
{
    gfx_round_rect(s, r, 4, 0x7A7A7A, 255);
    gfx_round_rect_vgradient(s, gfx_r(r.x + 1, r.y + 1, r.w - 2, r.h - 2), 3,
                             pressed ? 0xC8CDD6 : 0xFFFFFF, pressed ? 0xA9AFBA : 0xD9D9D9, 255);
}

/* Text clipped to r, showing its end when it doesn't fit (as fields do). */
static void field_text(gfx_surface *s, gfx_rect r, const gfx_font *f, const char *t, int n, uint32_t color)
{
    gfx_rect old = s->clip, c;

    if (!gfx_rect_intersect(old, r, &c))
        return;
    s->clip = c;
    int w = gfx_text_width_n(f, t, n);
    int x = w > r.w ? r.x + r.w - w : r.x;
    gfx_text_n(s, x, r.y + (r.h - f->line_height) / 2, f, t, n, color, 255);
    s->clip = old;
}

static void paint_ctl(gfx_surface *s, gfx_rect r, const struct ctl *c, bool focused)
{
    const struct style *st = c->node->st;
    const gfx_font *f = st->font;
    uint32_t ink = c->disabled ? 0x9A9A9A : 0x000000;

    switch (c->type) {
    case C_TEXT:
    case C_PASSWORD: {
        gfx_fill(s, r, 0xFFFFFF);
        frame(s, r, focused ? CTL_FOCUS : CTL_BORDER, focused ? 2 : 1);
        gfx_rect in = gfx_r(r.x + 4, r.y, r.w - 8, r.h);
        if (c->len == 0 && !focused) {
            const char *ph = node_attr(c->node, "placeholder");
            if (ph)
                field_text(s, in, f, ph, (int)strlen(ph), 0x9A9A9A);
        } else if (c->type == C_PASSWORD) {
            char dots[96];
            int n = 0;
            for (int i = 0; i < c->len && n + 1 < (int)sizeof(dots); i++)
                dots[n++] = GFX_CH_BULLET[0];
            field_text(s, in, f, dots, n, ink);
        } else {
            field_text(s, in, f, c->value, c->len, ink);
        }
        if (focused) {
            int w = gfx_text_width_n(f, c->value, c->len);
            int x = in.x + (w > in.w ? in.w : w) + 1;
            gfx_fill(s, gfx_r(x, r.y + 3, 2, r.h - 6), CTL_FOCUS);
        }
        break;
    }
    case C_TEXTAREA: {
        gfx_fill(s, r, 0xFFFFFF);
        frame(s, r, focused ? CTL_FOCUS : CTL_BORDER, focused ? 2 : 1);
        gfx_rect old = s->clip, cl;
        if (!gfx_rect_intersect(old, gfx_r(r.x + 3, r.y + 2, r.w - 6, r.h - 4), &cl))
            break;
        s->clip = cl;
        /* Greedy wrap at spaces or anywhere. */
        int y = r.y + 3, i = 0, maxw = r.w - 8;
        while (i < c->len && y < r.y + r.h) {
            int start = i, last_space = -1, j = i;
            while (j < c->len && c->value[j] != '\n' &&
                   gfx_text_width_n(f, c->value + start, j - start + 1) <= maxw) {
                if (c->value[j] == ' ')
                    last_space = j;
                j++;
            }
            int end = j;
            if (j < c->len && c->value[j] != '\n' && last_space > start)
                end = last_space + 1;
            if (end == start)
                end = start + 1;
            gfx_text_n(s, r.x + 4, y, f, c->value + start, end - start, ink, 255);
            i = end;
            if (i < c->len && c->value[i] == '\n')
                i++;
            y += f->line_height;
        }
        s->clip = old;
        break;
    }
    case C_CHECKBOX:
        gfx_fill(s, r, 0xFFFFFF);
        frame(s, r, focused ? CTL_FOCUS : CTL_BORDER, 1);
        if (c->checked)
            gfx_text_center(s, r.x, r.y + (r.h - f->line_height) / 2, r.w, f, GFX_CH_CHECK, 0x1A4FA8, 255);
        break;
    case C_RADIO: {
        int cx = r.x + r.w / 2, cy = r.y + r.h / 2, rad = r.w / 2;
        gfx_circle(s, cx, cy, rad, CTL_BORDER, 255);
        gfx_circle(s, cx, cy, rad - 1, 0xFFFFFF, 255);
        if (c->checked)
            gfx_circle(s, cx, cy, rad - 4, 0x1A4FA8, 255);
        break;
    }
    case C_SELECT: {
        button_face(s, r, focused);
        const char *label = ctl_label(c);
        field_text(s, gfx_r(r.x + 6, r.y, r.w - 24, r.h), f, label, (int)strlen(label), ink);
        int ax = r.x + r.w - 13, ay = r.y + r.h / 2 - 2;
        for (int k = 0; k < 4; k++)
            gfx_fill(s, gfx_r(ax + k, ay + k, 8 - 2 * k, 1), 0x333333);
        break;
    }
    case C_HIDDEN:
        break;
    default: {
        button_face(s, r, focused);
        const char *label = ctl_label(c);
        gfx_text_center(s, r.x, r.y + (r.h - f->line_height) / 2, r.w, f, label, ink, 255);
        break;
    }
    }
}

static void paint_image(struct page *p, gfx_surface *s, gfx_rect r, int img)
{
    struct image_slot *slot = img >= 0 && img < p->nimgs ? &p->imgs[img] : NULL;

    if (slot && slot->state == IMG_OK) {
        if (slot->w == r.w && slot->h == r.h)
            gfx_draw_image(s, r.x, r.y, &slot->img, 255);
        else
            gfx_draw_image_scaled(s, r, &slot->img, 255);
        return;
    }
    if (r.w < 4 || r.h < 4)
        return;
    gfx_fill(s, r, 0xF2F2F2);
    frame(s, r, 0xC4C4C4, 1);
}

void paint_page(struct page *p, gfx_surface *s, gfx_rect view, int scroll, gfx_rect clip,
                const struct node *hilite, const struct ctl *focus)
{
    gfx_rect area, saved = s->clip;

    if (!gfx_rect_intersect(view, clip, &area))
        return;
    gfx_set_clip(s, area);
    gfx_fill(s, area, p->bg);
    int top = scroll + (area.y - view.y), bottom = top + area.h;
    int dx = view.x, dy = view.y - scroll;

    for (int i = 0; i < p->nitems; i++) {
        const struct ditem *d = &p->items[i];
        if (d->y >= bottom || d->y + d->h <= top)
            continue;
        gfx_rect r = gfx_r(dx + d->x, dy + d->y, d->w, d->h);
        switch (d->kind) {
        case DI_RECT:
            gfx_fill(s, r, d->color);
            break;
        case DI_TEXT: {
            const gfx_font *f = d->u.t.f;
            gfx_text_n(s, r.x, r.y, f, d->u.t.s, d->u.t.n, d->color, 255);
            if (d->deco & DECO_UNDERLINE)
                gfx_fill(s, gfx_r(r.x, r.y + f->ascent + 1, d->w, 1), d->color);
            if (d->deco & DECO_STRIKE)
                gfx_fill(s, gfx_r(r.x, r.y + f->ascent * 2 / 3, d->w, 1), d->color);
            if (d->deco & DECO_OVERLINE)
                gfx_fill(s, gfx_r(r.x, r.y, d->w, 1), d->color);
            break;
        }
        case DI_IMAGE:
            paint_image(p, s, r, d->u.img);
            break;
        case DI_BULLET:
            if (d->u.bullet == LS_SQUARE) {
                gfx_fill(s, r, d->color);
            } else {
                int rad = d->w / 2;
                gfx_circle(s, r.x + rad, r.y + rad, rad, d->color, 255);
                if (d->u.bullet == LS_CIRCLE && rad > 1)
                    gfx_circle(s, r.x + rad, r.y + rad, rad - 1, p->bg, 255);
            }
            break;
        case DI_CTL:
            paint_ctl(s, r, d->u.ctl, d->u.ctl == focus);
            break;
        }
    }
    /* The pressed link: a translucent dark box over its pieces. */
    if (hilite) {
        for (int i = 0; i < p->nitems; i++) {
            const struct ditem *d = &p->items[i];
            if (d->link != hilite || d->y >= bottom || d->y + d->h <= top)
                continue;
            gfx_round_rect(s, gfx_r(dx + d->x - 2, dy + d->y - 1, d->w + 4, d->h + 2), 3, 0x202020, 80);
        }
    }
    s->clip = saved;
}
