/*
 * Style: selector matching and the cascade.
 *
 * Each element's style starts from its parent's inherited values, then
 * takes the UA sheet, HTML presentational attributes (bgcolor, align,
 * <font>, ...), author sheets in document order, the style attribute, and
 * finally !important author declarations. Within an origin, declarations
 * apply in specificity, then source, order.
 */

#include "web.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* The UA sheet, in CSS px (16px text). */
static const char ua_css[] =
    "html, body, address, blockquote, center, dd, details, dialog, dir, div, dl, dt, fieldset,"
    " figcaption, figure, footer, form, h1, h2, h3, h4, h5, h6, header, hgroup, hr, legend,"
    " listing, main, menu, nav, ol, p, plaintext, pre, section, article, aside, summary, ul, xmp,"
    " optgroup, frameset, frame { display: block }\n"
    "head, script, style, title, meta, link, base, template, area, map, datalist, param, iframe,"
    " object, embed, svg, canvas, video, audio, source, track, noembed, math, dialog,"
    " select option, select optgroup, input[type=hidden], col, colgroup { display: none }\n"
    "li { display: list-item }\n"
    "table { display: table; border-spacing: 2px }\n"
    "thead, tbody, tfoot { display: table-row-group }\n"
    "tr { display: table-row }\n"
    "td, th { display: table-cell; padding: 1px }\n"
    "caption { display: table-caption; text-align: center }\n"
    "input, select, textarea, button, img { display: inline-block }\n"
    "body { margin: 8px }\n"
    "h1 { font-size: 2em; margin: .67em 0; font-weight: bold }\n"
    "h2 { font-size: 1.5em; margin: .83em 0; font-weight: bold }\n"
    "h3 { font-size: 1.17em; margin: 1em 0; font-weight: bold }\n"
    "h4 { margin: 1.33em 0; font-weight: bold }\n"
    "h5 { font-size: .83em; margin: 1.67em 0; font-weight: bold }\n"
    "h6 { font-size: .67em; margin: 2.33em 0; font-weight: bold }\n"
    "p, dl, figure, menu, dir { margin: 1em 0 }\n"
    "blockquote { margin: 1em 20px }\n"
    "figure { margin: 1em 20px }\n"
    "ul, ol, menu, dir { margin: 1em 0; padding-left: 28px }\n"
    "ul { list-style-type: disc }\n"
    "ol { list-style-type: decimal }\n"
    "ul ul, ol ul, menu ul, ul menu { list-style-type: circle }\n"
    "ul ul ul, ul ol ul, ol ul ul, ol ol ul { list-style-type: square }\n"
    "li ul, li ol, ul ul, ol ol, ul ol, ol ul { margin-top: 0; margin-bottom: 0 }\n"
    "dd { margin-left: 28px }\n"
    "b, strong, th, dt, legend, summary { font-weight: bold }\n"
    "i, em, cite, var, dfn, address { font-style: italic }\n"
    "u, ins { text-decoration: underline }\n"
    "s, strike, del { text-decoration: line-through }\n"
    "a:link { color: #0000EE; text-decoration: underline }\n"
    "pre, code, tt, kbd, samp, xmp, listing, plaintext, textarea { font-family: monospace }\n"
    "pre, xmp, listing, plaintext { white-space: pre; margin: 1em 0 }\n"
    "textarea { white-space: pre-wrap }\n"
    "nobr { white-space: nowrap }\n"
    "small { font-size: smaller }\n"
    "big { font-size: larger }\n"
    "sub { vertical-align: sub; font-size: smaller }\n"
    "sup { vertical-align: super; font-size: smaller }\n"
    "center { text-align: center }\n"
    "th { text-align: center }\n"
    "td, th, tr { vertical-align: middle }\n"
    "hr { border: 1px inset #999; margin: .5em 0 }\n"
    "mark { background-color: #FF0; color: black }\n"
    "fieldset { margin: .5em 2px; padding: .35em .6em .6em; border: 2px groove #CCC }\n"
    "legend { padding: 0 2px }\n"
    "abbr[title], acronym[title] { text-decoration: none }\n"
    "q { font-style: normal }\n";

const gfx_font *web_font(int px, bool bold, bool italic, bool mono)
{
    static const int8_t reg[] = { 9, 10, 11, 12, 13, 14, 17, 24, 44 };
    static const int8_t bld[] = { 8, 9, 10, 11, 12, 13, 14, 17, 20 };
    static const int8_t ita[] = { 10, 11, 12, 13, 14 };
    static const int8_t mon[] = { 10, 11, 12 };
    const int8_t *sizes;
    int n, first;

    if (px < 9)
        px = 9;
    if (mono) {
        sizes = mon, n = 3, first = FONT_M10;
    } else if (bold) {
        sizes = bld, n = 9, first = FONT_B8;
    } else if (italic && px <= 16) {
        sizes = ita, n = 5, first = FONT_I10;
    } else {
        sizes = reg, n = 9, first = FONT_R9;
        if (px < 34 && px > 24)
            px = 24;                /* 44 is for clocks */
    }
    int best = 0;
    for (int i = 1; i < n; i++)
        if (abs(sizes[i] - px) < abs(sizes[best] - px))
            best = i;
    return ui_font(first + best);
}

struct stylesheet *style_ua_sheet(struct arena *unused)
{
    static struct arena ua_arena;
    static struct stylesheet ua;

    if (!ua.loaded)
        css_parse_sheet(&ua_arena, &ua, ua_css, (int)sizeof(ua_css) - 1);
    return &ua;
}

/* ---- matching ---------------------------------------------------------------- */

static struct node *prev_elem(const struct node *n)
{
    for (struct node *s = n->prev; s; s = s->prev)
        if (s->type == N_ELEM)
            return s;
    return NULL;
}

static struct node *next_elem(const struct node *n)
{
    for (struct node *s = n->next; s; s = s->next)
        if (s->type == N_ELEM)
            return s;
    return NULL;
}

static bool attr_match(const struct css_attrsel *a, const struct node *n)
{
    const char *v = node_attr(n, a->name);

    if (!v)
        return false;
    if (a->op == AO_EXISTS)
        return true;
    size_t vl = strlen(v), al = strlen(a->value);
    switch (a->op) {
    case AO_EQ:
        /* type="text" and the like are case-insensitive in HTML. */
        return !strcasecmp(v, a->value);
    case AO_PREFIX:
        return al && vl >= al && !strncmp(v, a->value, al);
    case AO_SUFFIX:
        return al && vl >= al && !strcmp(v + vl - al, a->value);
    case AO_SUBSTR:
        return al && strstr(v, a->value);
    case AO_DASH:
        return !strncmp(v, a->value, al) && (v[al] == 0 || v[al] == '-');
    case AO_WORD:
        for (const char *p = v; *p;) {
            while (*p == ' ')
                p++;
            const char *s = p;
            while (*p && *p != ' ')
                p++;
            if ((size_t)(p - s) == al && al && !strncmp(s, a->value, al))
                return true;
        }
        return false;
    }
    return false;
}

static bool match_compound(const struct css_compound *c, const struct node *n)
{
    if (n->type != N_ELEM)
        return false;
    if (c->tag ? n->tag != c->tag : c->tagname && (n->tag || strcmp(n->name, c->tagname)))
        return false;
    if (c->pseudo) {
        if (c->pseudo & PS_NEVER)
            return false;
        if ((c->pseudo & PS_LINK) && !(n->tag == T_A && node_attr(n, "href")))
            return false;
        if ((c->pseudo & PS_FIRST_CHILD) && prev_elem(n))
            return false;
        if ((c->pseudo & PS_LAST_CHILD) && next_elem(n))
            return false;
        if ((c->pseudo & PS_ROOT) && n->tag != T_HTML)
            return false;
    }
    if (c->id) {
        const char *id = node_attr(n, "id");
        if (!id || strcmp(id, c->id))
            return false;
    }
    for (int i = 0; i < c->ncls; i++)
        if (!node_has_class(n, c->cls[i], (int)strlen(c->cls[i])))
            return false;
    for (int i = 0; i < c->nattr; i++)
        if (!attr_match(&c->attrs[i], n))
            return false;
    return true;
}

static bool match_from(const struct css_selector *s, int i, const struct node *n)
{
    if (!match_compound(&s->parts[i], n))
        return false;
    if (i == 0)
        return true;
    switch (s->parts[i].comb) {
    case SC_CHILD:
        return n->parent && n->parent->type == N_ELEM && match_from(s, i - 1, n->parent);
    case SC_ADJ: {
        struct node *p = prev_elem(n);
        return p && match_from(s, i - 1, p);
    }
    case SC_SIB:
        for (struct node *p = prev_elem(n); p; p = prev_elem(p))
            if (match_from(s, i - 1, p))
                return true;
        return false;
    default:
        for (struct node *a = n->parent; a && a->type == N_ELEM; a = a->parent)
            if (match_from(s, i - 1, a))
                return true;
        return false;
    }
}

struct match {
    uint32_t spec, order;
    const struct css_rule *rule;
};

#define MAX_MATCHES 256

struct matches {
    struct match m[MAX_MATCHES];
    int n;
};

static void probe(const struct stylesheet *ss, uint32_t order_base, uint32_t key, const struct node *n,
                  struct matches *M)
{
    int lo = 0, hi = ss->nindex;

    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (ss->index[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (int i = lo; i < ss->nindex && ss->index[i].key == key; i++) {
        const struct css_rule *r = &ss->rules[ss->index[i].rule];
        const struct css_selector *sel = &r->sels[ss->index[i].sel];
        if (M->n >= MAX_MATCHES || !match_from(sel, sel->nparts - 1, n))
            continue;
        M->m[M->n].spec = sel->spec;
        M->m[M->n].order = order_base + (uint32_t)ss->index[i].rule;
        M->m[M->n].rule = r;
        M->n++;
    }
}

static uint32_t hash_n(const char *s, int n)
{
    uint32_t h = 2166136261u;

    for (int i = 0; i < n; i++)
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

static void collect(const struct stylesheet *ss, uint32_t order_base, const struct node *n, struct matches *M)
{
    if (!ss->nindex)
        return;
    const char *id = node_attr(n, "id");
    if (id && *id)
        probe(ss, order_base, css_key(3, css_hash(id)), n, M);
    const char *c = node_attr(n, "class");
    if (c) {
        /* Each distinct class once. */
        uint32_t seen[16];
        int nseen = 0;
        while (*c) {
            while (*c && isspace((unsigned char)*c))
                c++;
            const char *s = c;
            while (*c && !isspace((unsigned char)*c))
                c++;
            if (c == s)
                break;
            uint32_t h = hash_n(s, (int)(c - s));
            bool dup = false;
            for (int i = 0; i < nseen; i++)
                dup |= seen[i] == h;
            if (dup)
                continue;
            if (nseen < 16)
                seen[nseen++] = h;
            probe(ss, order_base, css_key(2, h), n, M);
        }
    }
    probe(ss, order_base, css_key(1, n->tag ? n->tag : css_hash(n->name) | 0x10000), n, M);
    probe(ss, order_base, css_key(0, 0), n, M);
}

static void sort_matches(struct matches *M, int from)
{
    for (int i = from + 1; i < M->n; i++) {
        struct match x = M->m[i];
        int j = i;
        while (j > from && (M->m[j - 1].spec > x.spec || (M->m[j - 1].spec == x.spec && M->m[j - 1].order > x.order))) {
            M->m[j] = M->m[j - 1];
            j--;
        }
        M->m[j] = x;
    }
}

/* ---- values -------------------------------------------------------------------- */

/* Per-element scratch for values resolved at the end. */
struct cascade {
    const struct page *p;
    struct style *st;
    const struct style *parent;
    int bstyle[4];              /* K_* border styles */
    bool bcolor_set[4];
    int display_kw;
    bool overflow_hidden, abs_pos;
    int left, top;              /* CSS px when positioned */
    bool left_set, top_set;
    bool float_set;
};

#define MAX_CSS_PX  10000

/* A length in CSS px * 64, relative to font size f4 (CSS px * 4). */
static int css_px64(const struct css_value *v, int f4)
{
    int n = v->num;

    if (n > MAX_CSS_PX * 64)
        n = MAX_CSS_PX * 64;
    if (n < -MAX_CSS_PX * 64)
        n = -MAX_CSS_PX * 64;
    if (v->type == V_NUMBER)
        return n;                   /* unitless: px, as quirks mode does */
    switch (v->unit) {
    case U_EM: return n * f4 / 4;
    case U_REM: return n * 16;
    case U_EX: case U_CH: return n * f4 / 8;
    case U_PT: return n * 4 / 3;
    case U_PC: return n * 16;
    case U_IN: return n * 96;
    case U_CM: return n * 3780 / 100;
    case U_MM: return n * 378 / 100;
    case U_VW: return n * WEB_VIEW_CSS_W / 100;
    case U_VH: return n * 405 / 100;
    default: return n;
    }
}

static int dev_px(const struct css_value *v, int f4)
{
    int px64 = css_px64(v, f4);
    return (px64 * 3 + (px64 >= 0 ? 128 : -128)) / 256;
}

static bool to_len(const struct css_value *v, int f4, struct len *out, bool allow_auto, bool allow_neg)
{
    if (v->type == V_AUTO || (v->type == V_KEYWORD && v->num == K_NONE_)) {
        if (!allow_auto)
            return false;
        out->unit = L_AUTO;
        out->v = 0;
        return true;
    }
    if (v->type == V_PERCENT) {
        int p = v->num;
        if (p > 32000)
            p = 32000;
        if (p < 0 && !allow_neg)
            p = 0;
        out->unit = L_PCT;
        out->v = (int16_t)p;
        return true;
    }
    if (v->type == V_LENGTH || v->type == V_NUMBER) {
        int px = dev_px(v, f4);
        if (px < 0 && !allow_neg)
            px = 0;
        if (px < -1000)
            px = -1000;
        out->unit = L_PX;
        out->v = (int16_t)px;
        return true;
    }
    return false;
}

static int display_of(int kw)
{
    switch (kw) {
    case K_NONE_: return D_NONE;
    case K_INLINE: case K_CONTENTS: return D_INLINE;
    case K_INLINE_BLOCK: case K_INLINE_FLEX: case K_INLINE_GRID: return D_INLINE_BLOCK;
    case K_LIST_ITEM: return D_LIST_ITEM;
    case K_TABLE: case K_INLINE_TABLE: return D_TABLE;
    case K_TABLE_ROW_GROUP: case K_TABLE_HEADER_GROUP: case K_TABLE_FOOTER_GROUP: return D_TABLE_ROW_GROUP;
    case K_TABLE_ROW: return D_TABLE_ROW;
    case K_TABLE_CELL: return D_TABLE_CELL;
    case K_TABLE_CAPTION: return D_TABLE_CAPTION;
    case K_TABLE_COLUMN: case K_TABLE_COLUMN_GROUP: return D_NONE;
    default: return D_BLOCK;
    }
}

static int list_style_of(int kw)
{
    switch (kw) {
    case K_NONE_: return LS_NONE;
    case K_CIRCLE: return LS_CIRCLE;
    case K_SQUARE: return LS_SQUARE;
    case K_DECIMAL: return LS_DECIMAL;
    case K_LOWER_ALPHA: case K_LOWER_LATIN: return LS_LOWER_ALPHA;
    case K_UPPER_ALPHA: case K_UPPER_LATIN: return LS_UPPER_ALPHA;
    case K_LOWER_ROMAN: return LS_LOWER_ROMAN;
    case K_UPPER_ROMAN: return LS_UPPER_ROMAN;
    case K_DISC: return LS_DISC;
    default: return -1;
    }
}

static void set_font_size(struct cascade *C, const struct css_value *v)
{
    int pf = C->parent->font_css, f;

    if (v->type == V_KEYWORD) {
        switch (v->num) {
        case K_XX_SMALL: f = 9 * 4; break;
        case K_X_SMALL: f = 10 * 4; break;
        case K_SMALL: f = 13 * 4; break;
        case K_MEDIUM: f = 16 * 4; break;
        case K_LARGE: f = 18 * 4; break;
        case K_X_LARGE: f = 24 * 4; break;
        case K_XX_LARGE: f = 32 * 4; break;
        case K_SMALLER: f = pf * 5 / 6; break;
        case K_LARGER: f = pf * 6 / 5; break;
        default: return;
        }
    } else if (v->type == V_PERCENT) {
        f = pf * v->num / (100 * 64);
    } else if (v->type == V_LENGTH && (v->unit == U_EM || v->unit == U_EX || v->unit == U_CH)) {
        f = css_px64(v, pf) / 16;
    } else if (v->type == V_LENGTH) {
        f = css_px64(v, pf) / 16;
    } else {
        return;
    }
    if (f < 4 * 4)
        f = 4 * 4;
    if (f > 200 * 4)
        f = 200 * 4;
    C->st->font_css = (int16_t)f;
}

static void apply(struct cascade *C, const struct css_decl *d)
{
    struct style *st = C->st;
    const struct style *ps = C->parent;
    const struct css_value *v = &d->v;
    int f4 = st->font_css;
    int kw = v->type == V_KEYWORD ? v->num : 0;

    if (v->type == V_INHERIT) {
        switch (d->prop) {
        case P_COLOR: st->color = ps->color; break;
        case P_BACKGROUND_COLOR: st->bg = ps->bg; break;
        case P_FONT_SIZE: st->font_css = ps->font_css; break;
        case P_DISPLAY: break;
        default:
            if (d->prop >= P_MARGIN_TOP && d->prop <= P_MARGIN_LEFT)
                st->margin[d->prop - P_MARGIN_TOP] = ps->margin[d->prop - P_MARGIN_TOP];
            else if (d->prop >= P_PADDING_TOP && d->prop <= P_PADDING_LEFT)
                st->padding[d->prop - P_PADDING_TOP] = ps->padding[d->prop - P_PADDING_TOP];
            else if (d->prop == P_WIDTH)
                st->width = ps->width;
            break;
        }
        return;
    }
    switch (d->prop) {
    case P_DISPLAY:
        if (kw)
            C->display_kw = kw;
        break;
    case P_COLOR:
        if (v->type == V_COLOR && v->num)
            st->color = (uint32_t)v->num & 0xFFFFFF;
        else if (kw == K_CURRENTCOLOR)
            st->color = ps->color;
        break;
    case P_BACKGROUND_COLOR:
        if (v->type == V_COLOR)
            st->bg = (uint32_t)v->num;
        else if (kw == K_CURRENTCOLOR)
            st->bg = 0xFF000000u | st->color;
        break;
    case P_FONT_SIZE:
        set_font_size(C, v);
        break;
    case P_FONT_WEIGHT:
        st->bold = kw == K_BOLD;
        break;
    case P_FONT_STYLE:
        if (kw == K_ITALIC || kw == K_OBLIQUE || kw == K_NORMAL)
            st->italic = kw != K_NORMAL;
        break;
    case P_FONT_FAMILY:
        st->mono = kw == K_MONOSPACE;
        break;
    case P_TEXT_ALIGN:
        if (kw == K_LEFT || kw == K_START)
            st->text_align = TA_LEFT;
        else if (kw == K_RIGHT || kw == K_END)
            st->text_align = TA_RIGHT;
        else if (kw == K_CENTER)
            st->text_align = TA_CENTER;
        else if (kw == K_JUSTIFY)
            st->text_align = TA_JUSTIFY;
        break;
    case P_TEXT_DECORATION:
        if (v->type == V_NUMBER)
            st->deco = (uint8_t)v->num;
        break;
    case P_TEXT_INDENT:
        to_len(v, f4, &st->text_indent, false, true);
        break;
    case P_TEXT_TRANSFORM:
        st->transform = kw == K_UPPERCASE ? TT_UPPER : kw == K_LOWERCASE ? TT_LOWER
                      : kw == K_CAPITALIZE ? TT_CAPITALIZE : TT_NONE;
        break;
    case P_WHITE_SPACE:
        st->white_space = kw == K_NOWRAP ? WS_NOWRAP : kw == K_PRE ? WS_PRE
                        : kw == K_PRE_WRAP || kw == K_BREAK_SPACES ? WS_PRE_WRAP
                        : kw == K_PRE_LINE ? WS_PRE_LINE : WS_NORMAL;
        break;
    case P_LINE_HEIGHT: {
        int fpx = (f4 * 3 + 8) / 16;
        if (kw == K_NORMAL) {
            st->line_h = 0;
        } else if (v->type == V_NUMBER) {
            st->line_h = (int16_t)(fpx * v->num / 64);
        } else if (v->type == V_PERCENT) {
            st->line_h = (int16_t)(fpx * v->num / (100 * 64));
        } else if (v->type == V_LENGTH) {
            st->line_h = (int16_t)dev_px(v, f4);
        }
        if (st->line_h > fpx * 3)
            st->line_h = (int16_t)(fpx * 3);
        if (st->line_h < 0)
            st->line_h = 0;
        break;
    }
    case P_MARGIN_TOP: case P_MARGIN_RIGHT: case P_MARGIN_BOTTOM: case P_MARGIN_LEFT:
        to_len(v, f4, &st->margin[d->prop - P_MARGIN_TOP], true, false);
        break;
    case P_PADDING_TOP: case P_PADDING_RIGHT: case P_PADDING_BOTTOM: case P_PADDING_LEFT:
        to_len(v, f4, &st->padding[d->prop - P_PADDING_TOP], false, false);
        break;
    case P_BORDER_TOP_WIDTH: case P_BORDER_RIGHT_WIDTH: case P_BORDER_BOTTOM_WIDTH: case P_BORDER_LEFT_WIDTH: {
        int side = d->prop - P_BORDER_TOP_WIDTH, w = -1;
        if (kw == K_THIN)
            w = 1;
        else if (kw == K_MEDIUM)
            w = 2;
        else if (kw == K_THICK)
            w = 4;
        else if (v->type == V_LENGTH || v->type == V_NUMBER) {
            w = dev_px(v, f4);
            if (w <= 0 && v->num > 0)
                w = 1;
        }
        if (w >= 0)
            st->border_w[side] = (int16_t)(w > 20 ? 20 : w);
        break;
    }
    case P_BORDER_TOP_STYLE: case P_BORDER_RIGHT_STYLE: case P_BORDER_BOTTOM_STYLE: case P_BORDER_LEFT_STYLE:
        if (kw)
            C->bstyle[d->prop - P_BORDER_TOP_STYLE] = kw;
        break;
    case P_BORDER_TOP_COLOR: case P_BORDER_RIGHT_COLOR: case P_BORDER_BOTTOM_COLOR: case P_BORDER_LEFT_COLOR: {
        int side = d->prop - P_BORDER_TOP_COLOR;
        if (v->type == V_COLOR) {
            st->border_c[side] = (uint32_t)v->num;
            C->bcolor_set[side] = true;
        } else if (kw == K_CURRENTCOLOR) {
            C->bcolor_set[side] = false;
        }
        break;
    }
    case P_WIDTH:
        to_len(v, f4, &st->width, true, false);
        break;
    case P_MAX_WIDTH:
        to_len(v, f4, &st->max_width, true, false);
        break;
    case P_MIN_WIDTH:
        to_len(v, f4, &st->min_width, true, false);
        break;
    case P_HEIGHT:
        to_len(v, f4, &st->height, true, false);
        break;
    case P_LIST_STYLE_TYPE: {
        int ls = list_style_of(kw);
        if (ls >= 0)
            st->list_style = (uint8_t)ls;
        break;
    }
    case P_VERTICAL_ALIGN:
        st->valign = kw == K_SUB ? VA_SUB : kw == K_SUPER ? VA_SUPER : kw == K_TOP || kw == K_TEXT_TOP ? VA_TOP
                   : kw == K_MIDDLE ? VA_MIDDLE : kw == K_BOTTOM || kw == K_TEXT_BOTTOM ? VA_BOTTOM : VA_BASELINE;
        break;
    case P_VISIBILITY:
        if (kw == K_HIDDEN || kw == K_COLLAPSE || kw == K_VISIBLE)
            st->hidden = kw != K_VISIBLE;
        break;
    case P_FLOAT:
        C->float_set = kw == K_LEFT || kw == K_RIGHT;
        break;
    case P_POSITION:
        C->abs_pos = kw == K_ABSOLUTE || kw == K_FIXED;
        break;
    case P_LEFT:
    case P_TOP:
        if (v->type == V_LENGTH || v->type == V_NUMBER) {
            int px = css_px64(v, f4) / 64;
            if (d->prop == P_LEFT) {
                C->left = px;
                C->left_set = true;
            } else {
                C->top = px;
                C->top_set = true;
            }
        }
        break;
    case P_OVERFLOW:
        C->overflow_hidden = kw == K_HIDDEN || kw == K_CLIP;
        break;
    }
}

static void apply_matches(struct cascade *C, const struct matches *M, int from, bool important)
{
    for (int i = from; i < M->n; i++) {
        const struct css_rule *r = M->m[i].rule;
        for (int k = 0; k < r->ndecls; k++)
            if (r->decls[k].important == important)
                apply(C, &r->decls[k]);
    }
}

/* ---- presentational attributes --------------------------------------------- */

/* "100" and "100px" are CSS px, "50%" a percentage. */
static bool hint_len(const char *v, struct len *out)
{
    if (!v)
        return false;
    while (*v == ' ')
        v++;
    if (!isdigit((unsigned char)*v))
        return false;
    int n = atoi(v);
    while (isdigit((unsigned char)*v))
        v++;
    if (*v == '.')
        for (v++; isdigit((unsigned char)*v); v++)
            ;
    if (*v == '%') {
        out->unit = L_PCT;
        out->v = (int16_t)(n > 500 ? 500 * 64 : n * 64);
    } else {
        out->unit = L_PX;
        out->v = (int16_t)CSS_TO_DEV(n > MAX_CSS_PX ? MAX_CSS_PX : n);
    }
    return true;
}

static bool hint_color(const char *v, uint32_t *out)
{
    return v && css_parse_color(v, (int)strlen(v), out) && *out;
}

static const struct node *ancestor(const struct node *n, int tag)
{
    for (n = n->parent; n; n = n->parent)
        if (n->tag == tag)
            return n;
    return NULL;
}

static void hints(struct cascade *C, const struct node *n)
{
    struct style *st = C->st;
    const char *v;
    uint32_t c;

    if (node_attr(n, "hidden")) {
        C->display_kw = K_NONE_;
        return;
    }
    if ((v = node_attr(n, "bgcolor")) && hint_color(v, &c))
        st->bg = c;
    if ((v = node_attr(n, "align"))) {
        bool table = n->tag == T_TABLE;
        if (!strcasecmp(v, "center") || !strcasecmp(v, "middle")) {
            if (table || n->tag == T_HR) {
                st->margin[1].unit = st->margin[3].unit = L_AUTO;
            } else if (n->tag != T_IMG) {
                st->text_align = TA_CENTER;
            }
        } else if (!strcasecmp(v, "right")) {
            if (table || n->tag == T_HR) {
                st->margin[3].unit = L_AUTO;
            } else if (n->tag != T_IMG) {
                st->text_align = TA_RIGHT;
            }
        } else if (!strcasecmp(v, "left") && n->tag != T_IMG && !table) {
            st->text_align = TA_LEFT;
        } else if (!strcasecmp(v, "justify")) {
            st->text_align = TA_JUSTIFY;
        }
    }
    if (n->tag == T_A && C->p->link_color && node_attr(n, "href"))
        st->color = C->p->link_color;
    switch (n->tag) {
    case T_BODY:
        if ((v = node_attr(n, "text")) && hint_color(v, &c))
            st->color = c & 0xFFFFFF;
        if ((v = node_attr(n, "marginwidth")) || (v = node_attr(n, "leftmargin")))
            hint_len(v, &st->margin[1]), st->margin[3] = st->margin[1];
        if ((v = node_attr(n, "marginheight")) || (v = node_attr(n, "topmargin")))
            hint_len(v, &st->margin[0]), st->margin[2] = st->margin[0];
        break;
    case T_FONT:
        if ((v = node_attr(n, "color")) && hint_color(v, &c))
            st->color = c & 0xFFFFFF;
        if ((v = node_attr(n, "face")))
            st->mono = strstr(v, "ourier") || strstr(v, "ono");
        if ((v = node_attr(n, "size"))) {
            static const int8_t sizes[8] = { 10, 10, 13, 16, 18, 24, 32, 48 };
            int k = atoi(v + (*v == '+'));
            if (*v == '+' || *v == '-')
                k += 3;
            k = k < 1 ? 1 : k > 7 ? 7 : k;
            st->font_css = (int16_t)(sizes[k] * 4);
        }
        break;
    case T_TABLE: {
        if (hint_len(node_attr(n, "width"), &st->width) && st->width.unit == L_PX && st->width.v == 0)
            st->width.unit = L_AUTO;
        hint_len(node_attr(n, "height"), &st->height);
        v = node_attr(n, "border");
        if (v) {
            int b = *v ? atoi(v) : 1;
            if (b > 0) {
                for (int i = 0; i < 4; i++) {
                    st->border_w[i] = (int16_t)CSS_TO_DEV(b) ? (int16_t)CSS_TO_DEV(b) : 1;
                    C->bstyle[i] = K_OUTSET;
                    st->border_c[i] = 0xFF808080u;
                    C->bcolor_set[i] = true;
                }
            }
        }
        break;
    }
    case T_TD:
    case T_TH: {
        hint_len(node_attr(n, "width"), &st->width);
        hint_len(node_attr(n, "height"), &st->height);
        if (node_attr(n, "nowrap"))
            st->white_space = WS_NOWRAP;
        const struct node *t = ancestor(n, T_TABLE);
        if (t && (v = node_attr(t, "cellpadding"))) {
            struct len l;
            if (hint_len(v, &l) && l.unit == L_PX)
                for (int i = 0; i < 4; i++)
                    st->padding[i] = l;
        }
        if (t && (v = node_attr(t, "border")) && (!*v || atoi(v) > 0)) {
            for (int i = 0; i < 4; i++) {
                st->border_w[i] = 1;
                C->bstyle[i] = K_INSET;
                st->border_c[i] = 0xFFA0A0A0u;
                C->bcolor_set[i] = true;
            }
        }
    }
    // fall through
    case T_TR:
        if ((v = node_attr(n, "valign")))
            st->valign = !strcasecmp(v, "top") ? VA_TOP : !strcasecmp(v, "bottom") ? VA_BOTTOM
                       : !strcasecmp(v, "middle") || !strcasecmp(v, "center") ? VA_MIDDLE : VA_BASELINE;
        break;
    case T_IMG:
        hint_len(node_attr(n, "width"), &st->width);
        hint_len(node_attr(n, "height"), &st->height);
        if ((v = node_attr(n, "hspace")))
            hint_len(v, &st->margin[1]), st->margin[3] = st->margin[1];
        if ((v = node_attr(n, "vspace")))
            hint_len(v, &st->margin[0]), st->margin[2] = st->margin[0];
        if ((v = node_attr(n, "border")) && atoi(v) > 0)
            for (int i = 0; i < 4; i++) {
                st->border_w[i] = (int16_t)(CSS_TO_DEV(atoi(v)) ? CSS_TO_DEV(atoi(v)) : 1);
                C->bstyle[i] = K_SOLID;
            }
        break;
    case T_HR:
        hint_len(node_attr(n, "width"), &st->width);
        if ((v = node_attr(n, "size")) && atoi(v) > 0) {
            int h = CSS_TO_DEV(atoi(v)) > 1 ? CSS_TO_DEV(atoi(v)) : 1;
            st->border_w[0] = (int16_t)(h / 2 > 0 ? h / 2 : 1);
            st->border_w[2] = (int16_t)(h - st->border_w[0]);
        }
        if ((v = node_attr(n, "color")) && hint_color(v, &c))
            for (int i = 0; i < 4; i++) {
                st->border_c[i] = c;
                C->bcolor_set[i] = true;
                C->bstyle[i] = K_SOLID;
            }
        if (node_attr(n, "noshade"))
            for (int i = 0; i < 4; i++)
                C->bstyle[i] = K_SOLID;
        break;
    case T_UL:
    case T_OL:
    case T_LI:
        if ((v = node_attr(n, "type"))) {
            int ls = !strcmp(v, "1") ? LS_DECIMAL : !strcmp(v, "a") ? LS_LOWER_ALPHA : !strcmp(v, "A") ? LS_UPPER_ALPHA
                   : !strcmp(v, "i") ? LS_LOWER_ROMAN : !strcmp(v, "I") ? LS_UPPER_ROMAN
                   : !strcasecmp(v, "disc") ? LS_DISC : !strcasecmp(v, "circle") ? LS_CIRCLE
                   : !strcasecmp(v, "square") ? LS_SQUARE : -1;
            if (ls >= 0)
                st->list_style = (uint8_t)ls;
        }
        break;
    case T_PRE:
        if (node_attr(n, "wrap"))
            st->white_space = WS_PRE_WRAP;
        break;
    }
}

/* ---- computing ----------------------------------------------------------------- */

static void inherit(struct style *st, const struct style *ps)
{
    memset(st, 0, sizeof(*st));
    st->display = D_INLINE;
    st->white_space = ps->white_space;
    st->text_align = ps->text_align;
    st->list_style = ps->list_style;
    st->transform = ps->transform;
    st->deco = ps->deco;
    st->bold = ps->bold;
    st->italic = ps->italic;
    st->mono = ps->mono;
    st->hidden = ps->hidden;
    st->font_css = ps->font_css;
    st->line_h = ps->line_h;
    st->color = ps->color;
    st->max_width.unit = st->min_width.unit = L_AUTO;
    st->width.unit = st->height.unit = L_AUTO;
    for (int i = 0; i < 4; i++) {
        st->margin[i].unit = L_PX;
        st->padding[i].unit = L_PX;
        st->border_w[i] = 2;        /* medium, shown only with a style */
    }
    st->text_indent.unit = L_PX;
}

static void finish(struct cascade *C, const struct node *n)
{
    struct style *st = C->st;

    for (int i = 0; i < 4; i++) {
        int bs = C->bstyle[i];
        if (!bs || bs == K_NONE_ || bs == K_HIDDEN)
            st->border_w[i] = 0;
        if (!C->bcolor_set[i])
            st->border_c[i] = 0xFF000000u | st->color;
        if ((bs == K_INSET || bs == K_GROOVE) && (i == 0 || i == 3))
            st->border_c[i] = 0xFF000000u | ((st->border_c[i] >> 1) & 0x7F7F7F);  /* darker top-left */
    }
    int d = C->display_kw ? display_of(C->display_kw) : st->display;
    if (C->display_kw == 0 && n->tag == T_UNKNOWN)
        d = D_INLINE;
    /* Floats and flex/grid items are block-level ("blockified"). */
    if ((C->float_set || C->parent->flex) && (d == D_INLINE || d == D_INLINE_BLOCK) && n->tag != T_IMG &&
        n->tag != T_INPUT && n->tag != T_BUTTON && n->tag != T_SELECT && n->tag != T_TEXTAREA)
        d = D_BLOCK;
    st->flex = C->display_kw == K_FLEX || C->display_kw == K_GRID || C->display_kw == K_INLINE_FLEX ||
               C->display_kw == K_INLINE_GRID;
    /* Content parked off-screen or clipped to a pixel (skip links,
     * screen-reader text) stays hidden here too. */
    if (C->abs_pos && ((C->left_set && C->left <= -500) || (C->top_set && C->top <= -500)))
        d = D_NONE;
    if (C->overflow_hidden && st->height.unit == L_PX && st->height.v <= 1 &&
        (st->width.unit == L_PX && st->width.v <= 1))
        d = D_NONE;
    st->display = (uint8_t)d;
    int fpx = (st->font_css * 3 + 8) / 16;
    st->font = web_font(fpx, st->bold, st->italic, st->mono);
}

struct style_ctx {
    struct page *p;
    struct stylesheet *ua;
    struct matches M;
};

static void compute(struct style_ctx *S, struct node *n, const struct style *ps)
{
    struct cascade C;
    struct style *st = n->st;

    if (!st) {
        st = n->st = arena_alloc(&S->p->arena, sizeof(*st));
        if (!st) {
            S->p->oom = true;
            return;
        }
    }
    memset(&C, 0, sizeof(C));
    C.p = S->p;
    C.st = st;
    C.parent = ps;
    inherit(st, ps);

    struct matches *M = &S->M;
    M->n = 0;
    collect(S->ua, 0, n, M);
    sort_matches(M, 0);
    /* Font size first, since em lengths depend on it. */
    for (int i = 0; i < M->n; i++)
        for (int k = 0; k < M->m[i].rule->ndecls; k++)
            if (M->m[i].rule->decls[k].prop == P_FONT_SIZE)
                apply(&C, &M->m[i].rule->decls[k]);
    apply_matches(&C, M, 0, false);
    hints(&C, n);

    int author = M->n;
    for (int s = 0; s < S->p->nsheets; s++)
        if (S->p->sheets[s].ss.loaded)
            collect(&S->p->sheets[s].ss, (uint32_t)(s + 1) << 20, n, M);
    sort_matches(M, author);
    if (n->nsdecl < 0) {
        const char *sa = node_attr(n, "style");
        n->nsdecl = sa ? (int16_t)css_parse_decls(&S->p->arena, sa, (int)strlen(sa), &n->sdecl) : 0;
    }
    for (int i = author; i < M->n; i++)
        for (int k = 0; k < M->m[i].rule->ndecls; k++)
            if (M->m[i].rule->decls[k].prop == P_FONT_SIZE && !M->m[i].rule->decls[k].important)
                apply(&C, &M->m[i].rule->decls[k]);
    for (int k = 0; k < n->nsdecl; k++)
        if (n->sdecl[k].prop == P_FONT_SIZE && !n->sdecl[k].important)
            apply(&C, &n->sdecl[k]);
    apply_matches(&C, M, author, false);
    for (int k = 0; k < n->nsdecl; k++)
        if (!n->sdecl[k].important)
            apply(&C, &n->sdecl[k]);
    apply_matches(&C, M, author, true);
    for (int k = 0; k < n->nsdecl; k++)
        if (n->sdecl[k].important)
            apply(&C, &n->sdecl[k]);
    finish(&C, n);
}

static void style_children(struct style_ctx *S, struct node *n, int depth)
{
    for (struct node *c = n->first; c; c = c->next) {
        if (c->type == N_TEXT) {
            c->st = n->st;
            continue;
        }
        if (c->type != N_ELEM)
            continue;
        compute(S, c, n->st);
        if (!c->st)
            return;
        if (c->st->display != D_NONE && depth < HTML_MAX_DEPTH + 2)
            style_children(S, c, depth + 1);
    }
}

void style_tree(struct page *p, struct stylesheet *ua)
{
    static struct style root;
    struct style_ctx *S = malloc(sizeof(*S));

    if (!S || !p->html) {
        free(S);
        p->oom = true;
        return;
    }
    S->p = p;
    S->ua = ua;
    memset(&root, 0, sizeof(root));
    root.font_css = 16 * 4;
    root.color = 0x000000;
    root.list_style = LS_DISC;
    compute(S, p->html, &root);
    if (p->html->st)
        style_children(S, p->html, 1);
    free(S);

    /* The canvas takes the root's background, or the body's. */
    p->bg = 0xFFFFFF;
    if (p->html->st && p->html->st->bg)
        p->bg = p->html->st->bg & 0xFFFFFF;
    else if (p->body && p->body->st && p->body->st->bg)
        p->bg = p->body->st->bg & 0xFFFFFF;
    if (p->body && p->body->st && !(p->html->st && p->html->st->bg))
        p->body->st->bg = 0;
}
