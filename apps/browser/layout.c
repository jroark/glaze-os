/*
 * Layout: the styled DOM becomes a display list in content coordinates.
 *
 * Blocks stack vertically with sibling (and simple parent/first-child)
 * margin collapsing. Inline content fills line boxes word by word; inline
 * blocks, images and form controls are atoms on the line. Tables use the
 * automatic algorithm (column min/max widths); a table whose minimum
 * doesn't fit the screen is linearized, cell after cell, which reads better
 * at 240 px than a sideways scroll. Floats and positioning are ignored:
 * floats are laid out as blocks.
 *
 * Flows (one per nesting level) live in a pool rather than on the 64 KiB
 * stack.
 */

#include "web.h"
#include "frames.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

enum { A_TEXT, A_IMG, A_CTL, A_BOX, A_FRAME };

struct atom {
    uint8_t kind, deco, valign;
    bool hidden;
    int16_t x, w, h;
    int16_t asc, desc;          /* above / below the baseline */
    int16_t shift;              /* text: baseline offset (sub/sup) */
    uint32_t color, bg;
    struct node *link;
    struct node *node;
    struct frame_slot *frame;
    const struct style *st;
    const char *s;
    int n;
    int img;
    struct ctl *ctl;
    int r0, r1;                 /* A_BOX: items laid out at (0, 0) */
};

#define MAX_ATOMS   96
#define MAX_FLOWS   (HTML_MAX_DEPTH + 8)

struct flow {
    int x0, w;                  /* content box */
    int y;                      /* top of the next line */
    int margin;                 /* pending bottom margin of the last block */
    int collapse_top;           /* parent top margin a first child absorbs, or -1 */
    const struct style *st;     /* the block container */
    struct node *link;
    uint32_t bg;                /* background of the enclosing inline */
    struct atom a[MAX_ATOMS];
    int na, lx;
    int brk;                    /* atoms from here on follow the last break chance */
    bool space;                 /* a collapsible space is pending */
    bool first_line;
    char marker[16];            /* list marker for the first line */
    uint8_t marker_style;
};

struct box_info {
    int bg;                     /* item index or -1 */
    int border0, border1;       /* border items */
    int content0, content1;     /* content items */
    int y, h;
};

struct lay {
    struct page *p;
    struct node *event_node;
    struct flow *pool[MAX_FLOWS];
    int depth;
    int carry_margin;           /* bottom margin escaping the last box */
    /* The list item whose first line gets a marker. */
    struct node *marker_for;
    char marker[16];
    uint8_t marker_style;
};

/* ---- display list ------------------------------------------------------------- */

static int add_item(struct lay *L, int kind)
{
    struct page *p = L->p;

    if (p->nitems == p->item_cap) {
        int cap = p->item_cap ? p->item_cap * 2 : 1024;
        struct ditem *n = realloc(p->items, sizeof(*n) * (size_t)cap);
        if (!n) {
            p->oom = true;
            return -1;
        }
        p->items = n;
        p->item_cap = cap;
    }
    struct ditem *d = &p->items[p->nitems];
    memset(d, 0, sizeof(*d));
    d->kind = (uint8_t)kind;
    return p->nitems++;
}

static void add_rect(struct lay *L, int x, int y, int w, int h, uint32_t color)
{
    if (w <= 0 || h <= 0)
        return;
    int i = add_item(L, DI_RECT);
    if (i < 0)
        return;
    struct ditem *d = &L->p->items[i];
    d->x = (int16_t)x;
    d->y = y;
    d->w = (int16_t)w;
    d->h = (int16_t)(h > 32767 ? 32767 : h);
    d->color = color & 0xFFFFFF;
}

static void translate(struct lay *L, int r0, int r1, int dx, int dy)
{
    for (int i = r0; i < r1 && i < L->p->nitems; i++) {
        L->p->items[i].x = (int16_t)(L->p->items[i].x + dx);
        L->p->items[i].y += dy;
    }
}

static void add_anchor(struct lay *L, const char *name, int y)
{
    struct page *p = L->p;

    if (!name || !*name)
        return;
    if (p->nanchors == p->anchor_cap) {
        int cap = p->anchor_cap ? p->anchor_cap * 2 : 64;
        struct anchor *a = realloc(p->anchors, sizeof(*a) * (size_t)cap);
        if (!a)
            return;
        p->anchors = a;
        p->anchor_cap = cap;
    }
    p->anchors[p->nanchors].name = name;
    p->anchors[p->nanchors].y = y;
    p->nanchors++;
}

/* ---- lengths ------------------------------------------------------------------- */

static int resolve(const struct len *l, int base)
{
    if (l->unit == L_PX)
        return l->v;
    if (l->unit == L_PCT)
        return base * l->v / (100 * 64);
    return 0;
}

static int font_px(const struct style *st)
{
    return (st->font_css * 3 + 8) / 16;
}

/* Strut: the line box a block's own font gives. */
static void strut(const struct style *st, int *asc, int *desc)
{
    const gfx_font *f = st->font;
    int lh = f->line_height, a = f->ascent;

    if (st->line_h > lh) {
        int extra = st->line_h - lh;
        a += extra / 2;
        lh = st->line_h;
    }
    *asc = a;
    *desc = lh - a;
}

/* ---- flows ----------------------------------------------------------------------- */

static struct flow *flow_push(struct lay *L, const struct style *st, int x0, int w, int y, struct node *link)
{
    if (L->depth >= MAX_FLOWS)
        return NULL;
    if (!L->pool[L->depth]) {
        L->pool[L->depth] = malloc(sizeof(struct flow));
        if (!L->pool[L->depth]) {
            L->p->oom = true;
            return NULL;
        }
    }
    struct flow *F = L->pool[L->depth++];
    F->x0 = x0;
    F->w = w > 1 ? w : 1;
    F->y = y;
    F->margin = 0;
    F->collapse_top = -1;
    F->st = st;
    F->link = link;
    F->bg = 0;
    F->na = 0;
    F->lx = 0;
    F->brk = 0;
    F->space = false;
    F->first_line = true;
    F->marker[0] = 0;
    return F;
}

static void flow_pop(struct lay *L)
{
    L->depth--;
}

static int indent_of(const struct flow *F)
{
    return F->first_line ? resolve(&F->st->text_indent, F->w) : 0;
}

static void emit_marker(struct lay *L, struct flow *F, int baseline, int asc)
{
    const struct style *st = F->st;

    if (!F->marker[0] && F->marker_style == LS_NONE)
        return;
    if (F->marker_style == LS_DISC || F->marker_style == LS_CIRCLE || F->marker_style == LS_SQUARE) {
        int i = add_item(L, DI_BULLET);
        if (i < 0)
            return;
        struct ditem *d = &L->p->items[i];
        int size = font_px(st) / 3 + 2;
        if (size < 4)
            size = 4;
        d->x = (int16_t)(F->x0 - size - 5);
        d->y = baseline - st->font->ascent / 2 - size / 2 - 1;
        d->w = d->h = (int16_t)size;
        d->color = st->color;
        d->u.bullet = F->marker_style;
        (void)asc;
        return;
    }
    if (!F->marker[0])
        return;
    int i = add_item(L, DI_TEXT);
    if (i < 0)
        return;
    struct ditem *d = &L->p->items[i];
    int w = gfx_text_width(st->font, F->marker);
    d->x = (int16_t)(F->x0 - w - 4);
    d->y = baseline - st->font->ascent;
    d->w = (int16_t)w;
    d->h = (int16_t)st->font->line_height;
    d->color = st->color;
    d->u.t.f = st->font;
    d->u.t.s = arena_strndup(&L->p->arena, F->marker, (int)strlen(F->marker));
    d->u.t.n = (int)strlen(F->marker);
    if (!d->u.t.s)
        L->p->nitems--;
}

/* Ends the current line after its first n atoms (an empty line too, if
 * force); the rest start the next line. */
static void line_flush_n(struct lay *L, struct flow *F, int n, bool force)
{
    if (!n && !force)
        return;
    int asc, desc;
    strut(F->st, &asc, &desc);
    int tall = 0;               /* top/middle/bottom aligned atoms */
    for (int i = 0; i < n; i++) {
        struct atom *a = &F->a[i];
        if (a->valign == VA_TOP || a->valign == VA_MIDDLE || a->valign == VA_BOTTOM) {
            if (a->h > tall)
                tall = a->h;
            continue;
        }
        if (a->asc > asc)
            asc = a->asc;
        if (a->desc > desc)
            desc = a->desc;
    }
    if (asc + desc < tall)
        desc = tall - asc;
    int top = F->y, baseline = top + asc, lh = asc + desc;

    int used = n < F->na ? F->a[n].x : F->lx, off = indent_of(F);
    int slack = F->w - used;
    if (slack > 0) {
        if (F->st->text_align == TA_CENTER)
            off += slack / 2;
        else if (F->st->text_align == TA_RIGHT)
            off += slack;
    }
    if (F->collapse_top >= 0)
        F->collapse_top = -1;
    if (F->first_line)
        emit_marker(L, F, baseline, asc);

    /* Backgrounds first, then contents. */
    for (int i = 0; i < n; i++) {
        struct atom *a = &F->a[i];
        if (a->bg && !a->hidden && a->kind == A_TEXT) {
            const gfx_font *f = a->st->font;
            add_rect(L, F->x0 + off + a->x, baseline - f->ascent, a->w, f->line_height, a->bg);
        }
    }
    for (int i = 0; i < n; i++) {
        struct atom *a = &F->a[i];
        int x = F->x0 + off + a->x;
        int y = a->valign == VA_TOP ? top : a->valign == VA_MIDDLE ? top + (lh - a->h) / 2
              : a->valign == VA_BOTTOM ? top + lh - a->h : baseline - a->asc;
        if (a->kind == A_BOX) {
            translate(L, a->r0, a->r1, x, y);
            continue;
        }
        if (a->hidden)
            continue;
        int k = add_item(L, a->kind == A_TEXT ? DI_TEXT : a->kind == A_IMG ? DI_IMAGE : a->kind==A_FRAME ? DI_FRAME : DI_CTL);
        if (k < 0)
            return;
        struct ditem *d = &L->p->items[k];
        d->x = (int16_t)x;
        d->w = a->w;
        d->link = a->link;
        d->node = a->node;
        d->color = a->color;
        d->deco = a->deco;
        if (a->kind == A_TEXT) {
            const gfx_font *f = a->st->font;
            d->y = baseline - f->ascent + a->shift;
            d->h = (int16_t)f->line_height;
            d->u.t.f = f;
            d->u.t.s = a->s;
            d->u.t.n = a->n;
        } else {
            d->y = y;
            d->h = a->h;
            if (a->kind == A_IMG)
                d->u.img = a->img;
            else if(a->kind==A_FRAME)
                d->u.frame=a->frame;
            else
                d->u.ctl = a->ctl;
        }
    }
    F->y = top + lh;
    int rest = F->na - n;
    if (rest > 0) {
        int sx = F->a[n].x;
        memmove(F->a, F->a + n, sizeof(struct atom) * (size_t)rest);
        for (int i = 0; i < rest; i++)
            F->a[i].x = (int16_t)(F->a[i].x - sx);
        F->lx -= sx;
    } else {
        F->lx = 0;
    }
    F->na = rest;
    F->brk = 0;
    F->space = false;
    F->first_line = false;
    F->marker[0] = 0;
    F->marker_style = LS_NONE;
}

static void line_flush(struct lay *L, struct flow *F, bool force)
{
    line_flush_n(L, F, F->na, force);
}

static struct atom *new_atom(struct lay *L, struct flow *F, int w)
{
    int avail = F->w - indent_of(F);

    if (F->na >= MAX_ATOMS || (F->lx + w > avail && F->na))
        line_flush(L, F, false);
    struct atom *a = &F->a[F->na++];
    memset(a, 0, sizeof(*a));
    a->x = (int16_t)F->lx;
    a->w = (int16_t)w;
    a->link = F->link;
    a->node = L->event_node;
    a->bg = F->bg;
    F->lx += w;
    return a;
}

/* ---- text ---------------------------------------------------------------------------- */

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* Collapsed text for the node's white-space mode, cached on the node. */
static const char *collapsed(struct lay *L, struct node *t, const struct style *st, int *len)
{
    uint8_t key = (uint8_t)(1 + st->white_space + 8 * st->transform);

    if (t->ctext && t->ckey == key) {
        *len = t->clen;
        return t->ctext;
    }
    char *out = arena_alloc(&L->p->arena, (size_t)t->len + 1);
    if (!out) {
        L->p->oom = true;
        *len = 0;
        return "";
    }
    int o = 0;
    bool pre = st->white_space == WS_PRE || st->white_space == WS_PRE_WRAP;
    bool keep_nl = pre || st->white_space == WS_PRE_LINE;
    bool word_start = true;
    for (int i = 0; i < t->len; i++) {
        char c = t->text[i];
        if (c == '\r')
            continue;
        if (!pre && is_space(c) && !(keep_nl && c == '\n')) {
            if (o && out[o - 1] == ' ')
                continue;
            if (o && out[o - 1] == '\n' && keep_nl)
                continue;
            c = ' ';
        }
        if (keep_nl && c == '\n' && !pre)
            while (o && out[o - 1] == ' ')
                o--;
        if (st->transform == TT_UPPER || (st->transform == TT_CAPITALIZE && word_start))
            c = (char)toupper((unsigned char)c);
        else if (st->transform == TT_LOWER)
            c = (char)tolower((unsigned char)c);
        word_start = is_space(c);
        out[o++] = c;
    }
    out[o] = 0;
    t->ctext = out;
    t->clen = o;
    t->ckey = key;
    *len = o;
    return out;
}

static void text_metrics(const struct style *st, struct atom *a)
{
    const gfx_font *f = st->font;
    int asc = f->ascent, desc = f->line_height - f->ascent;

    if (st->line_h > f->line_height) {
        int extra = st->line_h - f->line_height;
        asc += extra / 2;
        desc += extra - extra / 2;
    }
    a->shift = 0;
    if (st->valign == VA_SUB) {
        a->shift = (int16_t)(font_px(st) / 3);
        desc += a->shift;
        asc -= a->shift;
    } else if (st->valign == VA_SUPER) {
        a->shift = (int16_t)-(font_px(st) / 2);
        asc -= a->shift;
        desc += a->shift;
    }
    a->asc = (int16_t)asc;
    a->desc = (int16_t)(desc > 0 ? desc : 0);
}

static void put_word(struct lay *L, struct flow *F, const struct style *st, const char *s, int n)
{
    const gfx_font *f = st->font;
    int w = gfx_text_width_n(f, s, n);
    int sw = F->space && F->lx > 0 ? gfx_text_width_n(f, " ", 1) : 0;
    int avail = F->w - indent_of(F);

    if (F->lx + sw + w > avail && F->lx > 0) {
        if (!sw && F->brk > 0 && F->brk < F->na) {
            line_flush_n(L, F, F->brk, false);  /* no space: the run moves on together */
        } else {
            line_flush(L, F, false);
            sw = 0;
        }
        avail = F->w;
    }
    if (sw || !F->na)
        F->brk = F->na;
    F->space = false;
    /* Merge with the previous run when the source is contiguous. */
    if (F->na && sw >= 0) {
        struct atom *p = &F->a[F->na - 1];
        if (p->kind == A_TEXT && p->st == st && p->link == F->link && p->bg == F->bg &&
            p->s + p->n + (sw ? 1 : 0) == s && (!sw || p->s[p->n] == ' ') && F->lx + sw + w <= avail) {
            p->n = (int)(s + n - p->s);
            p->w = (int16_t)(p->w + sw + w);
            F->lx += sw + w;
            return;
        }
    }
    if (w > avail) {
        /* A word wider than the line: break it anywhere. */
        while (n > 0) {
            int room = F->w - indent_of(F) - F->lx - sw, k = 0;
            while (k < n && gfx_text_width_n(f, s, k + 1) <= room)
                k++;
            if (!k) {
                if (F->lx > 0) {
                    line_flush(L, F, false);
                    sw = 0;
                    continue;
                }
                k = 1;
            }
            F->lx += sw;
            sw = 0;
            struct atom *a = new_atom(L, F, gfx_text_width_n(f, s, k));
            a->kind = A_TEXT;
            a->st = st;
            a->s = s;
            a->n = k;
            a->color = st->color;
            a->deco = st->deco;
            a->hidden = st->hidden;
            text_metrics(st, a);
            s += k;
            n -= k;
            if (n > 0)
                line_flush(L, F, false);
        }
        return;
    }
    F->lx += sw;
    struct atom *a = new_atom(L, F, w);
    a->kind = A_TEXT;
    a->st = st;
    a->s = s;
    a->n = n;
    a->color = st->color;
    a->deco = st->deco;
    a->hidden = st->hidden;
    text_metrics(st, a);
}

static void flow_text(struct lay *L, struct flow *F, struct node *t)
{
    const struct style *st = t->st;
    int len;
    const char *s = collapsed(L, t, st, &len);
    bool pre = st->white_space == WS_PRE || st->white_space == WS_PRE_WRAP;

    for (int i = 0; i < len;) {
        char c = s[i];
        if (c == '\n') {
            if (F->na)
                line_flush(L, F, false);
            else
                line_flush(L, F, true);
            i++;
            continue;
        }
        if (c == ' ' || c == '\t') {
            if (!pre) {
                F->space = true;
                i++;
                continue;
            }
            /* Preserved spaces are words of their own; tabs every 8 columns. */
            int j = i;
            while (j < len && (s[j] == ' ' || s[j] == '\t'))
                j++;
            int n = 0;
            for (int k = i; k < j; k++)
                n += s[k] == '\t' ? 4 : 1;
            static const char spaces[] = "                                ";
            while (n > 0) {
                int k = n > 32 ? 32 : n;
                int w = gfx_text_width_n(st->font, spaces, k);
                if (F->lx + w > F->w && F->lx > 0) {
                    line_flush(L, F, false);
                    break;      /* wrapped: the spaces go */
                }
                struct atom *a = new_atom(L, F, w);
                a->kind = A_TEXT;
                a->st = st;
                a->s = spaces;
                a->n = k;
                a->hidden = true;
                text_metrics(st, a);
                n -= k;
            }
            i = j;
            continue;
        }
        int j = i;
        while (j < len && s[j] != ' ' && s[j] != '\n' && s[j] != '\t')
            j++;
        put_word(L, F, st, s + i, j - i);
        i = j;
    }
}

/* ---- atoms: images, controls ------------------------------------------------------------ */

static void image_size(struct lay *L, struct node *n, int avail, int *w, int *h, bool *known)
{
    const struct style *st = n->st;
    struct image_slot *slot = n->img >= 0 && n->img < L->p->nimgs ? &L->p->imgs[n->img] : NULL;
    int iw = 0, ih = 0;

    if (slot && slot->state == IMG_OK) {
        iw = slot->w;
        ih = slot->h;
    }
    int sw = st->width.unit != L_AUTO ? resolve(&st->width, avail) : 0;
    int sh = st->height.unit == L_PX ? st->height.v : 0;
    *known = true;
    if (sw && sh) {
        *w = sw, *h = sh;
    } else if (sw) {
        *w = sw;
        *h = iw ? sw * ih / iw : sw;
        *known = iw || sh;
    } else if (sh) {
        *h = sh;
        *w = ih ? sh * iw / ih : sh;
        *known = ih != 0;
    } else if (iw) {
        *w = iw, *h = ih;
    } else {
        *known = false;
        *w = *h = 0;
    }
    int maxw = avail;
    if (st->max_width.unit != L_AUTO) {
        int mw = resolve(&st->max_width, avail);
        if (mw < maxw)
            maxw = mw;
    }
    if (*w > maxw && *w > 0) {
        *h = *h * maxw / *w;
        *w = maxw;
    }
}

static void flow_image(struct lay *L, struct flow *F, struct node *n)
{
    int w, h;
    bool known;
    struct image_slot *slot = n->img >= 0 && n->img < L->p->nimgs ? &L->p->imgs[n->img] : NULL;

    image_size(L, n, F->w, &w, &h, &known);
    if (!known && (!slot || slot->state == IMG_FAILED || slot->state == IMG_NONE)) {
        /* Broken or missing: the alt text instead. */
        const char *alt = node_attr(n, "alt");
        if (alt && *alt) {
            int len = (int)strlen(alt);
            char *s = arena_strndup(&L->p->arena, alt, len);
            if (!s)
                return;
            for (int i = 0; i < len;) {
                while (i < len && s[i] == ' ') {
                    F->space = true;
                    i++;
                }
                int j = i;
                while (j < len && s[j] != ' ')
                    j++;
                if (j > i)
                    put_word(L, F, n->st, s + i, j - i);
                i = j;
            }
        }
        return;
    }
    if (!known) {
        w = w ? w : 16;
        h = h ? h : 16;
    }
    if (w <= 0 || h <= 0)
        return;
    int ml = resolve(&n->st->margin[3], F->w), mr = resolve(&n->st->margin[1], F->w);
    if (F->space && F->lx > 0)
        F->lx += gfx_text_width_n(F->st->font, " ", 1);
    F->space = false;
    F->lx += ml;
    struct atom *a = new_atom(L, F, w);
    a->kind = A_IMG;
    a->img = n->img;
    a->h = (int16_t)h;
    a->asc = (int16_t)h;
    a->valign = n->st->valign;
    a->hidden = n->st->hidden;
    F->brk = F->na;
    F->lx += mr;
}

static void ctl_size(const struct ctl *c, const struct style *st, int avail, int *w, int *h)
{
    const gfx_font *f = st->font;
    int ch = gfx_text_width(f, "0");

    *h = f->line_height + 6;
    switch (c->type) {
    case C_CHECKBOX:
    case C_RADIO:
        *w = *h = 13;
        return;
    case C_SUBMIT:
    case C_RESET:
    case C_BUTTON:
    case C_FILE:
        *w = gfx_text_width(f, ctl_label(c)) + 16;
        break;
    case C_SELECT: {
        int best = 40;
        for (int i = 0; i < c->nopts; i++) {
            const char *t = c->opts[i]->ctext ? c->opts[i]->ctext : "";
            int tw = gfx_text_width(f, t);
            if (tw > best)
                best = tw;
        }
        *w = best + 26;
        break;
    }
    case C_TEXTAREA:
        *w = (c->size > 0 ? c->size : 20) * ch + 8;
        *h = (c->rows > 0 ? c->rows : 2) * f->line_height + 6;
        break;
    default:
        *w = (c->size > 0 ? c->size : 20) * ch + 8;
        break;
    }
    if (st->width.unit != L_AUTO) {
        int sw = resolve(&st->width, avail);
        if (sw > 0)
            *w = sw;
    }
    if (*w > avail)
        *w = avail;
}

static void flow_ctl(struct lay *L, struct flow *F, struct node *n)
{
    struct ctl *c = n->ctl;
    int w, h;

    if (!c || c->type == C_HIDDEN)
        return;
    ctl_size(c, n->st, F->w, &w, &h);
    if (F->space && F->lx > 0)
        F->lx += gfx_text_width_n(F->st->font, " ", 1);
    F->space = false;
    F->lx += 2;
    struct atom *a = new_atom(L, F, w);
    a->kind = A_CTL;
    a->ctl = c;
    a->h = (int16_t)h;
    /* The control's text baseline lines up with the line's. */
    a->asc = (int16_t)(c->type == C_CHECKBOX || c->type == C_RADIO ? h : n->st->font->ascent + 3);
    a->desc = (int16_t)(h - a->asc);
    a->hidden = n->st->hidden;
    F->brk = F->na;
    F->lx += 2;
}

/* ---- measuring (min/max content widths) ------------------------------------------- */

struct measure {
    int line, mn, mx;
};

static void measure_box(struct lay *L, struct node *n, int *mn, int *mx, int depth);

static void m_flush(struct measure *M)
{
    if (M->line > M->mx)
        M->mx = M->line;
    M->line = 0;
}

static void m_atom(struct measure *M, int w)
{
    M->line += w;
    if (w > M->mn)
        M->mn = w;
}

static bool block_level(const struct style *st)
{
    return st->display == D_BLOCK || st->display == D_LIST_ITEM || st->display == D_TABLE ||
           st->display == D_TABLE_ROW_GROUP || st->display == D_TABLE_ROW || st->display == D_TABLE_CELL ||
           st->display == D_TABLE_CAPTION;
}

static void measure_flow(struct lay *L, struct node *n, struct measure *M, int depth)
{
    for (struct node *c = n->first; c; c = c->next) {
        if (c->type == N_TEXT) {
            const struct style *st = c->st;
            int len;
            const char *s = collapsed(L, c, st, &len);
            bool pre = st->white_space == WS_PRE || st->white_space == WS_PRE_WRAP;
            for (int i = 0; i < len;) {
                if (s[i] == '\n') {
                    m_flush(M);
                    i++;
                    continue;
                }
                int j = i;
                while (j < len && s[j] != ' ' && s[j] != '\n')
                    j++;
                int w = gfx_text_width_n(st->font, s + i, j - i);
                if (j < len && s[j] == ' ') {
                    M->line += gfx_text_width_n(st->font, " ", 1);
                    j++;
                }
                if (pre)
                    M->mn = M->mn > M->line + w ? M->mn : M->line + w;
                m_atom(M, w);
                i = j;
            }
            continue;
        }
        if (c->type != N_ELEM || !c->st || c->st->display == D_NONE)
            continue;
        if (c->tag == T_BR) {
            m_flush(M);
        } else if (c->tag == T_IMG) {
            int w, h;
            bool known;
            image_size(L, c, 240, &w, &h, &known);
            m_atom(M, known ? w : 16);
        } else if (c->ctl) {
            int w, h;
            ctl_size(c->ctl, c->st, 240, &w, &h);
            m_atom(M, w + 4);
        } else if (block_level(c->st)) {
            m_flush(M);
            int cmn, cmx;
            measure_box(L, c, &cmn, &cmx, depth + 1);
            if (cmn > M->mn)
                M->mn = cmn;
            if (cmx > M->mx)
                M->mx = cmx;
        } else if (c->st->display == D_INLINE_BLOCK) {
            int cmn, cmx;
            measure_box(L, c, &cmn, &cmx, depth + 1);
            M->line += cmx;
            if (cmn > M->mn)
                M->mn = cmn;
        } else if (depth < HTML_MAX_DEPTH) {
            measure_flow(L, c, M, depth + 1);
        }
    }
}

static int px_or_0(const struct len *l)
{
    return l->unit == L_PX ? l->v : 0;
}

static int box_extra(const struct style *st)
{
    return st->border_w[1] + st->border_w[3] + px_or_0(&st->padding[1]) + px_or_0(&st->padding[3]);
}

static void measure_table(struct lay *L, struct node *t, int *mn, int *mx, int depth);

static void measure_box(struct lay *L, struct node *n, int *mn, int *mx, int depth)
{
    const struct style *st = n->st;
    int extra = box_extra(st) + px_or_0(&st->margin[1]) + px_or_0(&st->margin[3]);

    *mn = *mx = 0;
    if (depth > HTML_MAX_DEPTH)
        return;
    if (st->display == D_TABLE) {
        measure_table(L, n, mn, mx, depth);
    } else {
        struct measure M = { 0, 0, 0 };
        measure_flow(L, n, &M, depth);
        m_flush(&M);
        *mn = M.mn;
        *mx = M.mx;
    }
    if (st->width.unit == L_PX) {
        int w = st->width.v;
        *mx = w;
        if (*mn < w && n->tag != T_TD && n->tag != T_TH)
            *mn = w;
    }
    *mn += extra;
    *mx += extra;
    if (*mn > *mx)
        *mx = *mn;
}

/* ---- blocks --------------------------------------------------------------------------- */

static int layout_box(struct lay *L, struct node *n, int x, int y, int bw, int cb_w, struct node *link,
                      int collapse_top, struct box_info *bi);
static void flow_children(struct lay *L, struct flow *F, struct node *n, int depth);
static int layout_table(struct lay *L, struct node *t, int x, int y, int w);

static void list_marker(struct lay *L, struct node *li)
{
    const struct style *st = li->st;
    int index = 1;
    struct node *list = li->parent;

    L->marker_for = li;
    L->marker_style = st->list_style;
    L->marker[0] = 0;
    if (st->list_style < LS_DECIMAL)
        return;
    if (list && list->tag == T_OL && node_attr(list, "start"))
        index = atoi(node_attr(list, "start"));
    if (list) {
        for (struct node *c = list->first; c && c != li; c = c->next) {
            if (c->type == N_ELEM && c->st && c->st->display == D_LIST_ITEM) {
                const char *v = node_attr(c, "value");
                index = v ? atoi(v) + 1 : index + 1;
            }
        }
    }
    if (node_attr(li, "value"))
        index = atoi(node_attr(li, "value"));
    char *m = L->marker;
    switch (st->list_style) {
    case LS_LOWER_ALPHA:
    case LS_UPPER_ALPHA:
        if (index >= 1 && index <= 26 * 27) {
            int k = 0;
            char tmp[4];
            for (int v = index; v > 0; v = (v - 1) / 26)
                tmp[k++] = (char)((st->list_style == LS_LOWER_ALPHA ? 'a' : 'A') + (v - 1) % 26);
            while (k)
                *m++ = tmp[--k];
            *m++ = '.';
            *m = 0;
            return;
        }
        break;
    case LS_LOWER_ROMAN:
    case LS_UPPER_ROMAN:
        if (index >= 1 && index < 40) {
            static const char *const ones[] = { "", "i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix" };
            static const char *const tens[] = { "", "x", "xx", "xxx" };
            int k = 0;
            for (const char *p = tens[index / 10]; *p; p++)
                m[k++] = *p;
            for (const char *p = ones[index % 10]; *p; p++)
                m[k++] = *p;
            m[k++] = '.';
            m[k] = 0;
            if (st->list_style == LS_UPPER_ROMAN)
                for (int i = 0; i < k; i++)
                    m[i] = (char)toupper((unsigned char)m[i]);
            return;
        }
        break;
    }
    int k = 0;
    char tmp[12];
    bool neg = index < 0;
    unsigned v = neg ? (unsigned)-index : (unsigned)index;
    do {
        tmp[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && k < 10);
    if (neg)
        *m++ = '-';
    while (k)
        *m++ = tmp[--k];
    *m++ = '.';
    *m = 0;
}

/* A block-level child in normal flow. */
static void layout_block(struct lay *L, struct flow *F, struct node *n)
{
    const struct style *st = n->st;
    int cw = F->w;

    line_flush(L, F, false);
    int ml = resolve(&st->margin[3], cw), mr = resolve(&st->margin[1], cw);
    int mt = resolve(&st->margin[0], cw), mb = resolve(&st->margin[2], cw);
    int extra = box_extra(st);
    int bw;
    bool auto_w = st->width.unit == L_AUTO;

    if (st->display == D_TABLE && auto_w) {
        /* Tables shrink to fit. */
        int mn, mx;
        measure_box(L, n, &mn, &mx, 0);
        mx -= px_or_0(&st->margin[1]) + px_or_0(&st->margin[3]);
        bw = mx < cw - ml - mr ? mx : cw - ml - mr;
        auto_w = false;
    } else if (auto_w) {
        bw = cw - ml - mr;
    } else {
        bw = resolve(&st->width, cw) + extra;
    }
    if (st->max_width.unit != L_AUTO) {
        int m = resolve(&st->max_width, cw) + extra;
        if (bw > m)
            bw = m;
    }
    if (st->min_width.unit != L_AUTO) {
        int m = resolve(&st->min_width, cw) + extra;
        if (bw < m)
            bw = m;
    }
    if (bw > cw - ml - mr)
        bw = cw - ml - mr;          /* never wider than the screen */
    if (bw < extra + 1)
        bw = extra + 1;
    if (!auto_w || bw < cw - ml - mr) {
        bool la = st->margin[3].unit == L_AUTO, ra = st->margin[1].unit == L_AUTO;
        if (la && ra)
            ml = (cw - bw) / 2;
        else if (la)
            ml = cw - bw - mr;
    }

    int gap = F->margin > mt ? F->margin : mt;
    if (F->collapse_top >= 0)
        gap = gap > F->collapse_top ? gap - F->collapse_top : 0;
    int y = F->y + gap;
    bool own_top = st->border_w[0] || px_or_0(&st->padding[0]);

    if (st->display == D_LIST_ITEM)
        list_marker(L, n);
    const char *id = node_attr(n, "id");
    if (id)
        add_anchor(L, id, y);
    L->carry_margin = 0;
    int h = layout_box(L, n, F->x0 + ml, y, bw, cw, F->link, own_top ? -1 : mt, NULL);
    F->y = y + h;
    F->margin = mb > L->carry_margin ? mb : L->carry_margin;
    F->collapse_top = -1;
    F->space = false;
    L->carry_margin = 0;
}

/* An inline-block: shrink-to-fit width, laid out at (0, 0), then placed. */
static void flow_inline_block(struct lay *L, struct flow *F, struct node *n)
{
    const struct style *st = n->st;
    int mn, mx;
    int ml = resolve(&st->margin[3], F->w), mr = resolve(&st->margin[1], F->w);
    int bw;

    if (st->width.unit != L_AUTO) {
        bw = resolve(&st->width, F->w) + box_extra(st);
    } else {
        measure_box(L, n, &mn, &mx, 0);
        mx -= ml + mr;
        mn -= ml + mr;
        int avail = F->w - ml - mr;
        bw = mx < avail ? mx : (mn > avail ? mn : avail);
    }
    if (bw > F->w - ml - mr)
        bw = F->w - ml - mr;
    if (bw < 1)
        bw = 1;
    if (F->space && F->lx > 0)
        F->lx += gfx_text_width_n(F->st->font, " ", 1);
    F->space = false;
    /* Lay out first so the atom knows its height. */
    int r0 = L->p->nitems;
    int saved = L->carry_margin;
    int h = layout_box(L, n, 0, 0, bw, F->w, F->link, -1, NULL);
    L->carry_margin = saved;
    int r1 = L->p->nitems;
    int mt = resolve(&st->margin[0], F->w), mb = resolve(&st->margin[2], F->w);
    if (F->lx + ml + bw + mr > F->w && F->na)
        line_flush(L, F, false);
    F->lx += ml;
    struct atom *a = new_atom(L, F, bw);
    a->kind = A_BOX;
    a->r0 = r0;
    a->r1 = r1;
    a->h = (int16_t)(h + mt + mb);
    a->asc = (int16_t)(h + mt);
    a->desc = (int16_t)mb;
    a->valign = st->valign;
    /* Items sit at y = 0: shift down by the top margin now. */
    translate(L, r0, r1, 0, mt);
    F->brk = F->na;
    F->lx += mr;
}

static void flow_frame(struct lay *L, struct flow *F, struct node *n)
{
    if (n->frame < 0 || n->frame >= L->p->nframes)
        return;
    int w = n->st->width.unit != L_AUTO ? resolve(&n->st->width, F->w) : CSS_TO_DEV(300);
    int h = n->st->height.unit == L_PX ? n->st->height.v : CSS_TO_DEV(150);
    const char *v = node_attr(n, "width");
    if (v && n->st->width.unit == L_AUTO)
        w = strchr(v, '%') ? F->w * atoi(v) / 100 : CSS_TO_DEV(atoi(v));
    v = node_attr(n, "height");
    if (v && n->st->height.unit != L_PX)
        h = CSS_TO_DEV(atoi(v));
    if (w > F->w)
        w = F->w;
    if (w < 4)
        w = 4;
    if (h < 4)
        h = 4;
    if (h > 2048)
        h = 2048;
    bool block = block_level(n->st);
    if (block)
        line_flush(L, F, false);
    struct atom *a = new_atom(L, F, w);
    a->kind = A_FRAME;
    a->frame = &L->p->frames[n->frame];
    a->h = a->asc = (int16_t)h;
    a->valign = n->st->valign;
    if (block)
        line_flush(L, F, false);
}

static void flow_node(struct lay *L, struct flow *F, struct node *n, int depth)
{
    L->event_node = n;
    if (n->type == N_TEXT) {
        if (n->st && n->len)
            flow_text(L, F, n);
        return;
    }
    if (n->type != N_ELEM || !n->st || n->st->display == D_NONE)
        return;
    const struct style *st = n->st;
    if(n->tag==T_IFRAME || n->tag==T_FRAME) { flow_frame(L,F,n);return; }
    if (block_level(st)) {
        layout_block(L, F, n);
        return;
    }
    if (n->tag == T_BR) {
        line_flush(L, F, true);
        return;
    }
    if (n->tag == T_IMG) {
        flow_image(L, F, n);
        return;
    }
    if (n->ctl && n->tag != T_BUTTON) {
        flow_ctl(L, F, n);
        return;
    }
    if (st->display == D_INLINE_BLOCK && depth < HTML_MAX_DEPTH) {
        if (n->tag == T_BUTTON && n->ctl && !n->first) {
            flow_ctl(L, F, n);
            return;
        }
        flow_inline_block(L, F, n);
        return;
    }
    /* Inline element. */
    struct node *saved_link = F->link;
    uint32_t saved_bg = F->bg;
    if (n->tag == T_A && node_attr(n, "href"))
        F->link = n;
    if (n->tag == T_A && node_attr(n, "name"))
        add_anchor(L, node_attr(n, "name"), F->y);
    const char *id = node_attr(n, "id");
    if (id)
        add_anchor(L, id, F->y);
    if (st->bg)
        F->bg = st->bg;
    int pl = px_or_0(&st->padding[3]) + st->border_w[3] + px_or_0(&st->margin[3]);
    int pr = px_or_0(&st->padding[1]) + st->border_w[1] + px_or_0(&st->margin[1]);
    if (pl && F->lx + pl < F->w)
        F->lx += pl;
    if (n->tag == T_Q)
        put_word(L, F, st, "\"", 1);
    if (depth < HTML_MAX_DEPTH)
        flow_children(L, F, n, depth + 1);
    if (n->tag == T_Q) {
        bool sp = F->space;
        F->space = false;
        put_word(L, F, st, "\"", 1);
        F->space = sp;
    }
    if (pr && F->lx + pr <= F->w)
        F->lx += pr;
    F->link = saved_link;
    F->bg = saved_bg;
}

static void flow_children(struct lay *L, struct flow *F, struct node *n, int depth)
{
    for (struct node *c = n->first; c; c = c->next)
        flow_node(L, F, c, depth);
}

static int layout_box(struct lay *L, struct node *n, int x, int y, int bw, int cb_w, struct node *link,
                      int collapse_top, struct box_info *bi)
{
    const struct style *st = n->st;
    int bt = st->border_w[0], br = st->border_w[1], bb = st->border_w[2], bl = st->border_w[3];
    int pt = resolve(&st->padding[0], cb_w), pr = resolve(&st->padding[1], cb_w);
    int pb = resolve(&st->padding[2], cb_w), pl = resolve(&st->padding[3], cb_w);
    int bg = -1;

    if (st->bg && !st->hidden) {
        bg = add_item(L, DI_RECT);
        if (bg >= 0)
            L->p->items[bg].color = st->bg & 0xFFFFFF;
        if(bg>=0)L->p->items[bg].node=n;
    }
    int cx = x + bl + pl, cy = y + bt + pt;
    int cw = bw - bl - br - pl - pr;
    if (cw < 1)
        cw = 1;
    int content0 = L->p->nitems, ch;
    struct node *saved_marker = NULL;
    int carry = 0;

    if (st->display == D_TABLE) {
        ch = layout_table(L, n, cx, cy, cw);
    } else {
        struct flow *F = flow_push(L, st, cx, cw, cy, link);
        if (!F)
            return 0;
        F->collapse_top = collapse_top;
        if (L->marker_for == n) {
            memcpy(F->marker, L->marker, sizeof(F->marker));
            F->marker_style = L->marker_style;
            saved_marker = n;
            L->marker_for = NULL;
        } else {
            F->marker_style = LS_NONE;
        }
        flow_children(L, F, n, L->depth);
        line_flush(L, F, false);
        if (saved_marker && (F->marker[0] || F->marker_style != LS_NONE)) {
            /* An empty list item still shows its marker. */
            line_flush(L, F, true);
        }
        ch = F->y - cy;
        if (pb || bb)
            ch += F->margin;
        else
            carry = F->margin;
        flow_pop(L);
    }
    int content1 = L->p->nitems;
    if (st->height.unit == L_PX && st->height.v > ch)
        ch = st->height.v;
    if (ch < 0)
        ch = 0;
    int h = bt + pt + ch + pb + bb;
    if (bg >= 0) {
        struct ditem *d = &L->p->items[bg];
        d->x = (int16_t)x;
        d->y = y;
        d->w = (int16_t)bw;
        d->h = (int16_t)(h > 32767 ? 32767 : h);
    }
    int border0 = L->p->nitems;
    if (!st->hidden) {
        add_rect(L, x, y, bw, bt, st->border_c[0]);
        add_rect(L, x + bw - br, y, br, h, st->border_c[1]);
        add_rect(L, x, y + h - bb, bw, bb, st->border_c[2]);
        add_rect(L, x, y, bl, h, st->border_c[3]);
    }
    if (bi) {
        bi->bg = bg;
        bi->border0 = border0;
        bi->border1 = L->p->nitems;
        bi->content0 = content0;
        bi->content1 = content1;
        bi->y = y;
        bi->h = h;
    }
    L->carry_margin = carry;
    return h;
}

/* Stretches a laid-out box to a new height (table cells in a row). */
static void stretch_box(struct lay *L, const struct box_info *bi, int h, int valign)
{
    int dh = h - bi->h;

    if (dh <= 0)
        return;
    if (bi->bg >= 0)
        L->p->items[bi->bg].h = (int16_t)h;
    for (int i = bi->border0; i < bi->border1; i++) {
        struct ditem *d = &L->p->items[i];
        if (d->y == bi->y && d->h == bi->h)
            d->h = (int16_t)h;          /* left and right */
        else if (d->y > bi->y)
            d->y += dh;                 /* bottom */
    }
    if (valign == VA_MIDDLE || valign == VA_BASELINE)
        translate(L, bi->content0, bi->content1, 0, dh / 2);
    else if (valign == VA_BOTTOM)
        translate(L, bi->content0, bi->content1, 0, dh);
}

/* ---- tables -------------------------------------------------------------------------- */

#define MAX_COLS    48
#define MAX_CELLS   6000

struct tcell {
    struct node *n;
    int row, col, span;
    int mn, mx;
};

struct tgrid {
    struct tcell *cells;
    int ncells, nrows, ncols;
    struct node **rows;
    int row_cap;
    struct node *caption;
};

static bool is_cell(const struct node *c)
{
    return c->type == N_ELEM && c->st && c->st->display == D_TABLE_CELL;
}

static void add_row(struct tgrid *G, struct node *tr)
{
    if (G->nrows == G->row_cap) {
        int cap = G->row_cap ? G->row_cap * 2 : 32;
        struct node **r = realloc(G->rows, sizeof(*r) * (size_t)cap);
        if (!r)
            return;
        G->rows = r;
        G->row_cap = cap;
    }
    int col = 0;
    for (struct node *c = tr->first; c; c = c->next) {
        if (!is_cell(c) || G->ncells >= MAX_CELLS)
            continue;
        struct tcell *tc = &G->cells[G->ncells++];
        const char *cs = node_attr(c, "colspan");
        int span = cs ? atoi(cs) : 1;
        span = span < 1 ? 1 : span > MAX_COLS ? MAX_COLS : span;
        if (col + span > MAX_COLS)
            span = MAX_COLS - col;
        if (span < 1) {
            G->ncells--;
            break;
        }
        tc->n = c;
        tc->row = G->nrows;
        tc->col = col;
        tc->span = span;
        col += span;
    }
    if (col > G->ncols)
        G->ncols = col;
    G->rows[G->nrows++] = tr;
}

static bool build_grid(struct tgrid *G, struct node *t)
{
    memset(G, 0, sizeof(*G));
    G->cells = malloc(sizeof(struct tcell) * MAX_CELLS);
    if (!G->cells)
        return false;
    for (struct node *c = t->first; c; c = c->next) {
        if (c->type != N_ELEM || !c->st)
            continue;
        if (c->st->display == D_TABLE_CAPTION && !G->caption)
            G->caption = c;
        else if (c->st->display == D_TABLE_ROW)
            add_row(G, c);
        else if (c->st->display == D_TABLE_ROW_GROUP)
            for (struct node *r = c->first; r; r = r->next)
                if (r->type == N_ELEM && r->st && r->st->display == D_TABLE_ROW)
                    add_row(G, r);
    }
    return true;
}

static void free_grid(struct tgrid *G)
{
    free(G->cells);
    free(G->rows);
}

static int spacing_of(const struct node *t)
{
    const char *v = node_attr(t, "cellspacing");
    int s = v ? CSS_TO_DEV(atoi(v)) : 1;

    return s < 0 ? 0 : s > 20 ? 20 : s;
}

static void column_widths(struct lay *L, struct tgrid *G, int *cmin, int *cmax, int depth)
{
    for (int c = 0; c < G->ncols; c++)
        cmin[c] = cmax[c] = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < G->ncells; i++) {
            struct tcell *tc = &G->cells[i];
            if ((tc->span == 1) != (pass == 0))
                continue;
            if (pass == 0)
                measure_box(L, tc->n, &tc->mn, &tc->mx, depth + 1);
            int smin = 0, smax = 0;
            for (int k = 0; k < tc->span; k++) {
                smin += cmin[tc->col + k];
                smax += cmax[tc->col + k];
            }
            if (tc->mn > smin)
                for (int k = 0; k < tc->span; k++)
                    cmin[tc->col + k] += (tc->mn - smin + tc->span - 1) / tc->span;
            if (tc->mx > smax)
                for (int k = 0; k < tc->span; k++)
                    cmax[tc->col + k] += (tc->mx - smax + tc->span - 1) / tc->span;
        }
    }
    for (int c = 0; c < G->ncols; c++)
        if (cmax[c] < cmin[c])
            cmax[c] = cmin[c];
}

static void measure_table(struct lay *L, struct node *t, int *mn, int *mx, int depth)
{
    struct tgrid G;
    int cmin[MAX_COLS], cmax[MAX_COLS];

    *mn = *mx = 0;
    if (!build_grid(&G, t))
        return;
    column_widths(L, &G, cmin, cmax, depth);
    int sp = spacing_of(t) * (G.ncols + 1);
    for (int c = 0; c < G.ncols; c++) {
        *mn += cmin[c];
        *mx += cmax[c];
    }
    /* A table too wide for the screen is linearized: its minimum is its
     * widest cell. */
    int widest = 0;
    for (int i = 0; i < G.ncells; i++)
        if (G.cells[i].mn > widest)
            widest = G.cells[i].mn;
    if (*mn + sp > 240)
        *mn = widest;
    else
        *mn += sp;
    *mx += sp;
    free_grid(&G);
}

static int layout_table(struct lay *L, struct node *t, int x, int y, int w)
{
    struct tgrid G;
    int cmin[MAX_COLS], cmax[MAX_COLS], width[MAX_COLS];
    int y0 = y;

    if (!build_grid(&G, t))
        return 0;
    if (G.caption) {
        int h = layout_box(L, G.caption, x, y, w, w, NULL, -1, NULL);
        y += h;
    }
    if (!G.ncells) {
        free_grid(&G);
        return y - y0;
    }
    column_widths(L, &G, cmin, cmax, L->depth);
    int sp = spacing_of(t);
    int avail = w - sp * (G.ncols + 1);
    int smin = 0, smax = 0;
    for (int c = 0; c < G.ncols; c++) {
        smin += cmin[c];
        smax += cmax[c];
    }

    if (smin > avail || avail <= 0) {
        /* Linearize: each cell as a block, in order. */
        for (int i = 0; i < G.ncells; i++) {
            int saved = L->carry_margin;
            y += layout_box(L, G.cells[i].n, x, y, w, w, NULL, -1, NULL);
            L->carry_margin = saved;
        }
        free_grid(&G);
        return y - y0;
    }
    bool fixed_w = t->st->width.unit != L_AUTO;
    int target = fixed_w || smax > avail ? avail : smax;
    for (int c = 0; c < G.ncols; c++) {
        if (smax <= target)
            width[c] = cmax[c] + (smax ? (target - smax) * cmax[c] / smax : (target - smax) / G.ncols);
        else
            width[c] = cmin[c] + (smax > smin ? (target - smin) * (cmax[c] - cmin[c]) / (smax - smin) : 0);
    }
    int used = 0;
    for (int c = 0; c < G.ncols; c++)
        used += width[c];
    if (G.ncols && (smax <= target ? fixed_w : true))
        width[G.ncols - 1] += target - used;

    int colx[MAX_COLS + 1];
    colx[0] = x + sp;
    for (int c = 0; c < G.ncols; c++)
        colx[c + 1] = colx[c] + width[c] + sp;

    struct box_info *bis = malloc(sizeof(struct box_info) * (size_t)G.ncells);
    y += sp;
    int i = 0;
    for (int r = 0; r < G.nrows; r++) {
        struct node *tr = G.rows[r];
        int row_bg = -1;
        if (tr->st && tr->st->bg) {
            row_bg = add_item(L, DI_RECT);
            if (row_bg >= 0)
                L->p->items[row_bg].color = tr->st->bg & 0xFFFFFF;
        }
        int first = i, row_h = 0;
        for (; i < G.ncells && G.cells[i].row == r; i++) {
            struct tcell *tc = &G.cells[i];
            int cx = colx[tc->col], cw = colx[tc->col + tc->span] - sp - cx;
            int saved = L->carry_margin;
            struct box_info tmp, *bi = bis ? &bis[i] : &tmp;
            int h = layout_box(L, tc->n, cx, y, cw, cw, NULL, -1, bi);
            L->carry_margin = saved;
            if (h > row_h)
                row_h = h;
        }
        if (tr->st && tr->st->height.unit == L_PX && tr->st->height.v > row_h)
            row_h = tr->st->height.v;
        if (bis)
            for (int k = first; k < i; k++)
                stretch_box(L, &bis[k], row_h, G.cells[k].n->st->valign);
        if (row_bg >= 0) {
            struct ditem *d = &L->p->items[row_bg];
            d->x = (int16_t)x;
            d->y = y;
            d->w = (int16_t)w;
            d->h = (int16_t)row_h;
        }
        y += row_h + sp;
    }
    free(bis);
    free_grid(&G);
    return y - y0;
}

/* ---- entry points ----------------------------------------------------------------------- */

void page_layout(struct page *p, int width)
{
    if(frameset_layout(p,width))return;
    struct lay *L = calloc(1, sizeof(*L));

    p->nitems = 0;
    p->nanchors = 0;
    p->width = width;
    p->content_h = 0;
    if (!L || !p->html || !p->html->st || !p->body || !p->body->st) {
        free(L);
        return;
    }
    L->p = p;
    struct flow *F = flow_push(L, p->html->st, 0, width, 0, NULL);
    if (F) {
        flow_node(L, F, p->html, 0);
        line_flush(L, F, false);
        p->content_h = F->y + F->margin;
        flow_pop(L);
    }
    for (int i = 0; i < MAX_FLOWS; i++)
        free(L->pool[i]);
    free(L);
    for(int i=0;i<p->nitems;i++)if(p->items[i].kind==DI_FRAME) {
        struct ditem *d=&p->items[i];frame_geometry(d->u.frame,d->x+1,d->y+1,d->w>2 ? d->w-2 : 1,d->h>2 ? d->h-2 : 1);
    }
}

int page_anchor_y(struct page *p, const char *frag)
{
    if (!frag || !*frag)
        return 0;
    if (!strcmp(frag, "top"))
        return 0;
    for (int i = 0; i < p->nanchors; i++)
        if (!strcmp(p->anchors[i].name, frag))
            return p->anchors[i].y;
    return -1;
}

struct ditem *page_hit(struct page *p, int x, int y)
{
    struct ditem *best = NULL;
    int best_d = 10;                /* touch slop, px */

    for (int i = p->nitems - 1; i >= 0; i--) {
        struct ditem *d = &p->items[i];
        if(d->kind==DI_FRAME) {
            struct frame_slot *f=d->u.frame;
            if(f && f->page && x>=f->x && y>=f->y && x<f->x+f->w && y<f->y+f->h) {
                struct ditem *inside=page_hit(f->page,x-f->x,y-f->y+f->scroll);
                if(inside)return inside;
            }
        }
        if (!d->link && d->kind != DI_CTL && !d->node)
            continue;
        int dx = x < d->x ? d->x - x : x >= d->x + d->w ? x - (d->x + d->w - 1) : 0;
        int dy = y < d->y ? d->y - y : y >= d->y + d->h ? y - (d->y + d->h - 1) : 0;
        int dist = dx > dy ? dx : dy;
        if (dist == 0)
            return d;
        if(!d->link && d->kind!=DI_CTL)continue;
        if (dist < best_d) {
            best_d = dist;
            best = d;
        }
    }
    return best;
}
