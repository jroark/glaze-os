/*
 * CSS parsing: stylesheets, selectors and declarations.
 *
 * Unknown properties and values are dropped, as CSS requires. Shorthands
 * (margin, border, font, background, ...) expand into the longhands web.h
 * knows. @media blocks are kept when they match a 320 CSS px wide portrait
 * screen; @supports blocks are kept; other at-rules are skipped.
 */

#include "web.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_DECLS       64
#define MAX_SELECTORS   128     /* per rule; the UA sheet's first has ~50 */

struct cp {
    struct arena *a;
    const char *s;
    int len;
};

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* Skip whitespace and comments from i. */
static int skip_ws(const char *s, int len, int i)
{
    for (;;) {
        while (i < len && is_ws(s[i]))
            i++;
        if (i + 1 < len && s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/'))
                i++;
            i = i + 2 < len ? i + 2 : len;
            continue;
        }
        return i;
    }
}

/* From an opening '{' at i: index just past its matching '}'. */
static int block_end(const char *s, int len, int i)
{
    int depth = 0;

    for (; i < len; i++) {
        char c = s[i];
        if (c == '"' || c == '\'') {
            for (i++; i < len && s[i] != c; i++)
                if (s[i] == '\\')
                    i++;
        } else if (c == '/' && i + 1 < len && s[i + 1] == '*') {
            for (i += 2; i + 1 < len && !(s[i] == '*' && s[i + 1] == '/'); i++)
                ;
            i++;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            if (--depth == 0)
                return i + 1;
        }
    }
    return len;
}

/* ---- numbers, lengths, colours ------------------------------------------------ */

/* Parses a number as 1/64ths; returns chars used (0 if none). */
static int parse_num(const char *s, int len, int32_t *out)
{
    int i = 0;
    bool neg = false;
    int32_t ip = 0, frac = 0, scale = 1;

    if (i < len && (s[i] == '+' || s[i] == '-'))
        neg = s[i++] == '-';
    int digits = 0;
    while (i < len && isdigit((unsigned char)s[i])) {
        if (ip < 100000)
            ip = ip * 10 + (s[i] - '0');
        i++;
        digits++;
    }
    if (i < len && s[i] == '.' && i + 1 < len && isdigit((unsigned char)s[i + 1])) {
        i++;
        while (i < len && isdigit((unsigned char)s[i])) {
            if (scale < 100000) {
                frac = frac * 10 + (s[i] - '0');
                scale *= 10;
            }
            i++;
            digits++;
        }
    }
    if (!digits)
        return 0;
    if (i + 1 < len && (s[i] == 'e' || s[i] == 'E') && (isdigit((unsigned char)s[i + 1]) || s[i + 1] == '-')) {
        /* Exponents: rare; small ones only. */
        int j = i + 1;
        bool eneg = s[j] == '-';
        if (eneg || s[j] == '+')
            j++;
        int e = 0;
        while (j < len && isdigit((unsigned char)s[j]))
            e = e * 10 + (s[j++] - '0');
        if (e > 0 && e < 6) {
            for (int k = 0; k < e; k++) {
                if (eneg) {
                    frac = (ip % 10) * scale + frac;
                    scale *= 10;
                    ip /= 10;
                } else {
                    ip = ip * 10 + (frac * 10 / scale);
                    frac = frac * 10 % scale;
                }
            }
        }
        i = j;
    }
    int32_t v = ip * 64 + frac * 64 / scale;
    *out = neg ? -v : v;
    return i;
}

static const struct {
    const char *name;
    uint8_t unit;
} units[] = {
    { "px", U_PX }, { "em", U_EM }, { "rem", U_REM }, { "ex", U_EX }, { "pt", U_PT },
    { "pc", U_PC }, { "in", U_IN }, { "cm", U_CM }, { "mm", U_MM }, { "vw", U_VW },
    { "vh", U_VH }, { "ch", U_CH }, { "vmin", U_VW }, { "vmax", U_VH }, { "q", U_MM },
};

static bool parse_length(const char *s, int len, struct css_value *v)
{
    int32_t num;
    int n = parse_num(s, len, &num);

    if (!n)
        return false;
    if (n == len) {
        v->type = V_NUMBER;
        v->num = num;
        return true;
    }
    if (s[n] == '%' && n + 1 == len) {
        v->type = V_PERCENT;
        v->num = num;
        return true;
    }
    for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        if ((int)strlen(units[i].name) == len - n && !strncasecmp(s + n, units[i].name, (size_t)(len - n))) {
            v->type = V_LENGTH;
            v->unit = units[i].unit;
            v->num = num;
            return true;
        }
    }
    return false;
}

static const struct {
    const char *name;
    uint32_t rgb;
} named_colors[] = {
    { "black", 0x000000 }, { "silver", 0xC0C0C0 }, { "gray", 0x808080 }, { "grey", 0x808080 },
    { "white", 0xFFFFFF }, { "maroon", 0x800000 }, { "red", 0xFF0000 }, { "purple", 0x800080 },
    { "fuchsia", 0xFF00FF }, { "magenta", 0xFF00FF }, { "green", 0x008000 }, { "lime", 0x00FF00 },
    { "olive", 0x808000 }, { "yellow", 0xFFFF00 }, { "navy", 0x000080 }, { "blue", 0x0000FF },
    { "teal", 0x008080 }, { "aqua", 0x00FFFF }, { "cyan", 0x00FFFF }, { "orange", 0xFFA500 },
    { "pink", 0xFFC0CB }, { "brown", 0xA52A2A }, { "gold", 0xFFD700 }, { "darkgray", 0xA9A9A9 },
    { "darkgrey", 0xA9A9A9 }, { "lightgray", 0xD3D3D3 }, { "lightgrey", 0xD3D3D3 },
    { "darkblue", 0x00008B }, { "darkred", 0x8B0000 }, { "darkgreen", 0x006400 },
    { "lightblue", 0xADD8E6 }, { "lightgreen", 0x90EE90 }, { "lightyellow", 0xFFFFE0 },
    { "whitesmoke", 0xF5F5F5 }, { "gainsboro", 0xDCDCDC }, { "beige", 0xF5F5DC },
    { "ivory", 0xFFFFF0 }, { "khaki", 0xF0E68C }, { "coral", 0xFF7F50 }, { "tomato", 0xFF6347 },
    { "salmon", 0xFA8072 }, { "crimson", 0xDC143C }, { "indigo", 0x4B0082 }, { "violet", 0xEE82EE },
    { "orchid", 0xDA70D6 }, { "plum", 0xDDA0DD }, { "tan", 0xD2B48C }, { "wheat", 0xF5DEB3 },
    { "skyblue", 0x87CEEB }, { "steelblue", 0x4682B4 }, { "royalblue", 0x4169E1 },
    { "dodgerblue", 0x1E90FF }, { "slategray", 0x708090 }, { "slategrey", 0x708090 },
    { "dimgray", 0x696969 }, { "dimgrey", 0x696969 }, { "darkorange", 0xFF8C00 },
    { "firebrick", 0xB22222 }, { "forestgreen", 0x228B22 }, { "seagreen", 0x2E8B57 },
    { "midnightblue", 0x191970 }, { "aliceblue", 0xF0F8FF }, { "lavender", 0xE6E6FA },
    { "linen", 0xFAF0E6 }, { "snow", 0xFFFAFA }, { "honeydew", 0xF0FFF0 }, { "azure", 0xF0FFFF },
    { "ghostwhite", 0xF8F8FF }, { "oldlace", 0xFDF5E6 }, { "floralwhite", 0xFFFAF0 },
    { "cornsilk", 0xFFF8DC }, { "lemonchiffon", 0xFFFACD }, { "seashell", 0xFFF5EE },
    { "mistyrose", 0xFFE4E1 }, { "antiquewhite", 0xFAEBD7 }, { "papayawhip", 0xFFEFD5 },
    { "bisque", 0xFFE4C4 }, { "peachpuff", 0xFFDAB9 }, { "moccasin", 0xFFE4B5 },
    { "navajowhite", 0xFFDEAD }, { "palegoldenrod", 0xEEE8AA }, { "darkkhaki", 0xBDB76B },
    { "olivedrab", 0x6B8E23 }, { "yellowgreen", 0x9ACD32 }, { "darkolivegreen", 0x556B2F },
    { "darkslategray", 0x2F4F4F }, { "darkslategrey", 0x2F4F4F }, { "cadetblue", 0x5F9EA0 },
    { "darkcyan", 0x008B8B }, { "lightseagreen", 0x20B2AA }, { "aquamarine", 0x7FFFD4 },
    { "turquoise", 0x40E0D0 }, { "darkturquoise", 0x00CED1 }, { "deepskyblue", 0x00BFFF },
    { "lightskyblue", 0x87CEFA }, { "cornflowerblue", 0x6495ED }, { "lightsteelblue", 0xB0C4DE },
    { "mediumblue", 0x0000CD }, { "darkslateblue", 0x483D8B }, { "slateblue", 0x6A5ACD },
    { "rebeccapurple", 0x663399 }, { "darkviolet", 0x9400D3 }, { "darkorchid", 0x9932CC },
    { "darkmagenta", 0x8B008B }, { "mediumpurple", 0x9370DB }, { "thistle", 0xD8BFD8 },
    { "hotpink", 0xFF69B4 }, { "deeppink", 0xFF1493 }, { "lightpink", 0xFFB6C1 },
    { "lightcoral", 0xF08080 }, { "indianred", 0xCD5C5C }, { "darksalmon", 0xE9967A },
    { "lightsalmon", 0xFFA07A }, { "orangered", 0xFF4500 }, { "chocolate", 0xD2691E },
    { "sienna", 0xA0522D }, { "saddlebrown", 0x8B4513 }, { "peru", 0xCD853F },
    { "sandybrown", 0xF4A460 }, { "burlywood", 0xDEB887 }, { "rosybrown", 0xBC8F8F },
    { "goldenrod", 0xDAA520 }, { "darkgoldenrod", 0xB8860B }, { "lawngreen", 0x7CFC00 },
    { "chartreuse", 0x7FFF00 }, { "springgreen", 0x00FF7F }, { "palegreen", 0x98FB98 },
    { "limegreen", 0x32CD32 }, { "mediumseagreen", 0x3CB371 }, { "darkseagreen", 0x8FBC8F },
    { "lightslategray", 0x778899 }, { "lightslategrey", 0x778899 }, { "lightcyan", 0xE0FFFF },
    { "paleturquoise", 0xAFEEEE }, { "powderblue", 0xB0E0E6 }, { "mintcream", 0xF5FFFA },
    { "mediumvioletred", 0xC71585 }, { "palevioletred", 0xDB7093 }, { "greenyellow", 0xADFF2F },
    { "mediumaquamarine", 0x66CDAA }, { "mediumorchid", 0xBA55D3 }, { "mediumslateblue", 0x7B68EE },
    { "mediumspringgreen", 0x00FA9A }, { "mediumturquoise", 0x48D1CC }, { "blanchedalmond", 0xFFEBCD },
    { "lavenderblush", 0xFFF0F5 }, { "darkgoldenrod", 0xB8860B },
};

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* Colour with alpha (0-255) over white, as there's no compositing. */
static uint32_t over_white(uint32_t rgb, int a)
{
    if (a >= 255)
        return 0xFF000000u | rgb;
    if (a <= 0)
        return 0;
    int r = (int)(rgb >> 16 & 255), g = (int)(rgb >> 8 & 255), b = (int)(rgb & 255);
    r = (r * a + 255 * (255 - a)) / 255;
    g = (g * a + 255 * (255 - a)) / 255;
    b = (b * a + 255 * (255 - a)) / 255;
    return 0xFF000000u | (uint32_t)(r << 16 | g << 8 | b);
}

/* Up to 4 numeric arguments of rgb()/hsl(), with commas, spaces or '/'. */
static int func_args(const char *s, int len, int32_t *vals, bool *pct)
{
    int n = 0, i = 0;

    while (i < len && n < 4) {
        while (i < len && (is_ws(s[i]) || s[i] == ',' || s[i] == '/'))
            i++;
        if (i >= len)
            break;
        int k = parse_num(s + i, len - i, &vals[n]);
        if (!k)
            return -1;
        i += k;
        pct[n] = i < len && s[i] == '%';
        if (pct[n])
            i++;
        while (i < len && isalpha((unsigned char)s[i]))
            i++;                    /* "deg" */
        n++;
    }
    return n;
}

static int alpha_of(int32_t v, bool pct)
{
    int a = pct ? v * 255 / (100 * 64) : v * 255 / 64;
    return a < 0 ? 0 : a > 255 ? 255 : a;
}

static int clamp255(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

bool css_parse_color(const char *s, int len, uint32_t *out)
{
    while (len && is_ws(*s)) {
        s++;
        len--;
    }
    while (len && is_ws(s[len - 1]))
        len--;
    if (!len)
        return false;
    if (s[0] == '#' || (len == 6 && hexval(s[0]) >= 0 && hexval(s[5]) >= 0 && hexval(s[2]) >= 0)) {
        /* Also bare "rrggbb", which old HTML attributes use. */
        const char *h = s[0] == '#' ? s + 1 : s;
        int n = (int)(s + len - h);
        int d[8];
        for (int i = 0; i < n && i < 8; i++)
            if ((d[i] = hexval(h[i])) < 0)
                return false;
        if (n == 3 || n == 4) {
            uint32_t rgb = (uint32_t)(d[0] * 17 << 16 | d[1] * 17 << 8 | d[2] * 17);
            *out = over_white(rgb, n == 4 ? d[3] * 17 : 255);
            return true;
        }
        if (n == 6 || n == 8) {
            uint32_t rgb = (uint32_t)((d[0] << 4 | d[1]) << 16 | (d[2] << 4 | d[3]) << 8 | (d[4] << 4 | d[5]));
            *out = over_white(rgb, n == 8 ? d[6] << 4 | d[7] : 255);
            return true;
        }
        return false;
    }
    const char *paren = memchr(s, '(', (size_t)len);
    if (paren && s[len - 1] == ')') {
        int fl = (int)(paren - s);
        int32_t v[4];
        bool pct[4] = { false };
        int n = func_args(paren + 1, len - fl - 2, v, pct);
        if ((fl == 3 || fl == 4) && !strncasecmp(s, "rgb", 3) && n >= 3) {
            int c[3];
            for (int i = 0; i < 3; i++)
                c[i] = clamp255(pct[i] ? v[i] * 255 / (100 * 64) : v[i] / 64);
            *out = over_white((uint32_t)(c[0] << 16 | c[1] << 8 | c[2]), n > 3 ? alpha_of(v[3], pct[3]) : 255);
            return true;
        }
        if ((fl == 3 || fl == 4) && !strncasecmp(s, "hsl", 3) && n >= 3) {
            int h = (v[0] / 64 % 360 + 360) % 360;
            int sat = v[1] * 10 / 64, lig = v[2] * 10 / 64;     /* 0..1000 */
            sat = sat < 0 ? 0 : sat > 1000 ? 1000 : sat;
            lig = lig < 0 ? 0 : lig > 1000 ? 1000 : lig;
            int c = (1000 - abs(2 * lig - 1000)) * sat / 1000;
            int x = c * (1000 - abs(h * 1000 / 60 % 2000 - 1000)) / 1000;
            int m = lig - c / 2, r, g, b;
            switch (h / 60) {
            case 0: r = c; g = x; b = 0; break;
            case 1: r = x; g = c; b = 0; break;
            case 2: r = 0; g = c; b = x; break;
            case 3: r = 0; g = x; b = c; break;
            case 4: r = x; g = 0; b = c; break;
            default: r = c; g = 0; b = x; break;
            }
            uint32_t rgb = (uint32_t)(clamp255((r + m) * 255 / 1000) << 16 |
                                      clamp255((g + m) * 255 / 1000) << 8 | clamp255((b + m) * 255 / 1000));
            *out = over_white(rgb, n > 3 ? alpha_of(v[3], pct[3]) : 255);
            return true;
        }
        return false;
    }
    if (len == 11 && !strncasecmp(s, "transparent", 11)) {
        *out = 0;
        return true;
    }
    for (size_t i = 0; i < sizeof(named_colors) / sizeof(named_colors[0]); i++) {
        if ((int)strlen(named_colors[i].name) == len && !strncasecmp(s, named_colors[i].name, (size_t)len)) {
            *out = 0xFF000000u | named_colors[i].rgb;
            return true;
        }
    }
    return false;
}

/* ---- declarations -------------------------------------------------------------- */

struct tok {
    const char *s;
    int len;
};

/* Splits a value into space-separated tokens, keeping functions whole;
 * commas end a token too (and are dropped). */
static int tokens(const char *s, int len, struct tok *t, int max)
{
    int n = 0, i = 0;

    while (i < len && n < max) {
        while (i < len && (is_ws(s[i]) || s[i] == ','))
            i++;
        if (i >= len)
            break;
        int st = i, depth = 0;
        if (s[i] == '/') {
            t[n].s = s + i;
            t[n++].len = 1;
            i++;
            continue;
        }
        while (i < len) {
            char c = s[i];
            if (c == '(')
                depth++;
            else if (c == ')')
                depth--;
            else if (c == '"' || c == '\'') {
                for (i++; i < len && s[i] != c; i++)
                    ;
            } else if (depth <= 0 && (is_ws(c) || c == ',' || c == '/'))
                break;
            i++;
        }
        t[n].s = s + st;
        t[n++].len = i - st;
    }
    return n;
}

static bool tok_is(const struct tok *t, const char *w)
{
    return (int)strlen(w) == t->len && !strncasecmp(t->s, w, (size_t)t->len);
}

struct dlist {
    struct css_decl d[MAX_DECLS];
    int n;
    bool important;
};

static void add(struct dlist *L, int prop, struct css_value v)
{
    if (L->n >= MAX_DECLS)
        return;
    L->d[L->n].prop = (uint8_t)prop;
    L->d[L->n].important = L->important;
    L->d[L->n].v = v;
    L->n++;
}

static struct css_value kwv(int k)
{
    struct css_value v = { V_KEYWORD, 0, k };
    return v;
}

/* Generic value: length, percent, number, auto, inherit, colour or keyword. */
static bool value_of(const struct tok *t, struct css_value *v, bool want_color)
{
    memset(v, 0, sizeof(*v));
    if (tok_is(t, "inherit")) {
        v->type = V_INHERIT;
        return true;
    }
    if (tok_is(t, "auto")) {
        v->type = V_AUTO;
        return true;
    }
    if (want_color) {
        uint32_t c;
        if (tok_is(t, "currentcolor")) {
            *v = kwv(K_CURRENTCOLOR);
            return true;
        }
        if (css_parse_color(t->s, t->len, &c)) {
            v->type = V_COLOR;
            v->num = (int32_t)c;
            return true;
        }
        return false;
    }
    if (parse_length(t->s, t->len, v))
        return true;
    int k = kw_lookup(t->s, t->len);
    if (k) {
        *v = kwv(k);
        return true;
    }
    return false;
}

static bool is_border_style(int k)
{
    return k == K_NONE_ || k == K_HIDDEN || k == K_SOLID || k == K_DOTTED || k == K_DASHED ||
           k == K_DOUBLE || k == K_GROOVE || k == K_RIDGE || k == K_INSET || k == K_OUTSET;
}

/* One side's width, style and colour from a border shorthand. */
static void border_side(struct dlist *L, int side, struct tok *t, int n)
{
    struct css_value w = { V_KEYWORD, 0, K_MEDIUM }, st = kwv(K_NONE_), c = kwv(K_CURRENTCOLOR);

    for (int i = 0; i < n; i++) {
        struct css_value v;
        int k = kw_lookup(t[i].s, t[i].len);
        if (k && is_border_style(k))
            st = kwv(k);
        else if (k == K_THIN || k == K_MEDIUM || k == K_THICK)
            w = kwv(k);
        else if (parse_length(t[i].s, t[i].len, &v))
            w = v;
        else if (value_of(&t[i], &v, true))
            c = v;
    }
    add(L, P_BORDER_TOP_WIDTH + side, w);
    add(L, P_BORDER_TOP_STYLE + side, st);
    add(L, P_BORDER_TOP_COLOR + side, c);
}

/* 1-4 values onto top/right/bottom/left longhands starting at base. */
static void four(struct dlist *L, int base, struct tok *t, int n, bool color)
{
    struct css_value v[4];

    if (n < 1 || n > 4)
        return;
    for (int i = 0; i < n; i++)
        if (!value_of(&t[i], &v[i], color))
            return;
    if (n == 1)
        v[1] = v[2] = v[3] = v[0];
    else if (n == 2) {
        v[2] = v[0];
        v[3] = v[1];
    } else if (n == 3)
        v[3] = v[1];
    for (int i = 0; i < 4; i++)
        add(L, base + i, v[i]);
}

static bool font_size_token(const struct tok *t, struct css_value *v)
{
    int k = kw_lookup(t->s, t->len);

    if (k >= K_XX_SMALL && k <= K_LARGER) {
        *v = kwv(k);
        return true;
    }
    if (k == K_MEDIUM) {
        *v = kwv(k);
        return true;
    }
    return parse_length(t->s, t->len, v) && v->type != V_NUMBER;
}

static int family_kind(const char *s, int len)
{
    static const char *const mono[] = { "monospace", "courier", "mono", "consolas", "menlo", "monaco" };

    for (int i = 0; i < len; i++)
        for (size_t k = 0; k < sizeof(mono) / sizeof(mono[0]); k++) {
            int ml = (int)strlen(mono[k]);
            if (i + ml <= len && !strncasecmp(s + i, mono[k], (size_t)ml))
                return K_MONOSPACE;
        }
    return K_SANS;
}

static void weight(struct dlist *L, const struct tok *t)
{
    int32_t n;
    int k = kw_lookup(t->s, t->len);

    if (k == K_BOLD || k == K_BOLDER)
        add(L, P_FONT_WEIGHT, kwv(K_BOLD));
    else if (k == K_NORMAL || k == K_LIGHTER)
        add(L, P_FONT_WEIGHT, kwv(K_NORMAL));
    else if (parse_num(t->s, t->len, &n) == t->len)
        add(L, P_FONT_WEIGHT, kwv(n >= 600 * 64 ? K_BOLD : K_NORMAL));
}

static void font_shorthand(struct dlist *L, struct tok *t, int n)
{
    int i = 0;
    struct css_value v;

    /* Defaults the shorthand resets. */
    add(L, P_FONT_STYLE, kwv(K_NORMAL));
    add(L, P_FONT_WEIGHT, kwv(K_NORMAL));
    for (; i < n; i++) {
        int k = kw_lookup(t[i].s, t[i].len);
        int32_t num;
        if (k == K_ITALIC || k == K_OBLIQUE)
            add(L, P_FONT_STYLE, kwv(K_ITALIC));
        else if (k == K_BOLD || k == K_BOLDER || k == K_LIGHTER ||
                 (parse_num(t[i].s, t[i].len, &num) == t[i].len && num >= 100 * 64))
            weight(L, &t[i]);
        else if (k == K_NORMAL || tok_is(&t[i], "small-caps"))
            ;
        else
            break;
    }
    if (i >= n || !font_size_token(&t[i], &v))
        return;
    add(L, P_FONT_SIZE, v);
    i++;
    if (i + 1 < n && t[i].len == 1 && t[i].s[0] == '/') {
        if (value_of(&t[i + 1], &v, false))
            add(L, P_LINE_HEIGHT, v);
        i += 2;
    }
    if (i < n)
        add(L, P_FONT_FAMILY, kwv(family_kind(t[i].s, (int)(t[n - 1].s + t[n - 1].len - t[i].s))));
}

static const struct {
    const char *name;
    uint8_t prop;
    uint8_t color;
} simple_props[] = {
    { "display", P_DISPLAY, 0 }, { "color", P_COLOR, 1 }, { "background-color", P_BACKGROUND_COLOR, 1 },
    { "font-size", P_FONT_SIZE, 0 }, { "font-style", P_FONT_STYLE, 0 }, { "text-align", P_TEXT_ALIGN, 0 },
    { "text-indent", P_TEXT_INDENT, 0 }, { "text-transform", P_TEXT_TRANSFORM, 0 },
    { "white-space", P_WHITE_SPACE, 0 }, { "line-height", P_LINE_HEIGHT, 0 },
    { "margin-top", P_MARGIN_TOP, 0 }, { "margin-right", P_MARGIN_RIGHT, 0 },
    { "margin-bottom", P_MARGIN_BOTTOM, 0 }, { "margin-left", P_MARGIN_LEFT, 0 },
    { "padding-top", P_PADDING_TOP, 0 }, { "padding-right", P_PADDING_RIGHT, 0 },
    { "padding-bottom", P_PADDING_BOTTOM, 0 }, { "padding-left", P_PADDING_LEFT, 0 },
    { "border-top-width", P_BORDER_TOP_WIDTH, 0 }, { "border-right-width", P_BORDER_RIGHT_WIDTH, 0 },
    { "border-bottom-width", P_BORDER_BOTTOM_WIDTH, 0 }, { "border-left-width", P_BORDER_LEFT_WIDTH, 0 },
    { "border-top-style", P_BORDER_TOP_STYLE, 0 }, { "border-right-style", P_BORDER_RIGHT_STYLE, 0 },
    { "border-bottom-style", P_BORDER_BOTTOM_STYLE, 0 }, { "border-left-style", P_BORDER_LEFT_STYLE, 0 },
    { "border-top-color", P_BORDER_TOP_COLOR, 1 }, { "border-right-color", P_BORDER_RIGHT_COLOR, 1 },
    { "border-bottom-color", P_BORDER_BOTTOM_COLOR, 1 }, { "border-left-color", P_BORDER_LEFT_COLOR, 1 },
    { "width", P_WIDTH, 0 }, { "max-width", P_MAX_WIDTH, 0 }, { "min-width", P_MIN_WIDTH, 0 },
    { "height", P_HEIGHT, 0 }, { "max-height", P_MAX_HEIGHT, 0 },
    { "list-style-type", P_LIST_STYLE_TYPE, 0 }, { "list-style-position", P_LIST_STYLE_POSITION, 0 },
    { "vertical-align", P_VERTICAL_ALIGN, 0 }, { "visibility", P_VISIBILITY, 0 },
    { "float", P_FLOAT, 0 }, { "clear", P_CLEAR, 0 }, { "position", P_POSITION, 0 },
    { "left", P_LEFT, 0 }, { "top", P_TOP, 0 }, { "overflow", P_OVERFLOW, 0 },
    { "inline-size", P_WIDTH, 0 }, { "max-inline-size", P_MAX_WIDTH, 0 },
    { "margin-inline-start", P_MARGIN_LEFT, 0 }, { "margin-inline-end", P_MARGIN_RIGHT, 0 },
    { "padding-inline-start", P_PADDING_LEFT, 0 }, { "padding-inline-end", P_PADDING_RIGHT, 0 },
    { "margin-block-start", P_MARGIN_TOP, 0 }, { "margin-block-end", P_MARGIN_BOTTOM, 0 },
};

static void declaration(struct dlist *L, const char *name, int nl, const char *val, int vl)
{
    struct tok t[16];
    int n = tokens(val, vl, t, 16);
    struct css_value v;

    if (!n)
        return;
    /* Custom properties, var() and calc() are beyond this engine. */
    if (nl >= 2 && name[0] == '-' && name[1] == '-')
        return;
    for (int i = 0; i < n; i++)
        if (t[i].len > 4 && (!strncasecmp(t[i].s, "var(", 4) || !strncasecmp(t[i].s, "calc(", 5) ||
                             !strncasecmp(t[i].s, "env(", 4) || !strncasecmp(t[i].s, "min(", 4) ||
                             !strncasecmp(t[i].s, "max(", 4) || !strncasecmp(t[i].s, "clamp(", 6)))
            return;

#define IS(s) ((int)sizeof(s) - 1 == nl && !strncasecmp(name, s, (size_t)nl))
    if (IS("margin")) {
        four(L, P_MARGIN_TOP, t, n, false);
    } else if (IS("padding")) {
        four(L, P_PADDING_TOP, t, n, false);
    } else if (IS("border-width")) {
        four(L, P_BORDER_TOP_WIDTH, t, n, false);
    } else if (IS("border-style")) {
        four(L, P_BORDER_TOP_STYLE, t, n, false);
    } else if (IS("border-color")) {
        four(L, P_BORDER_TOP_COLOR, t, n, true);
    } else if (IS("border")) {
        for (int s = 0; s < 4; s++)
            border_side(L, s, t, n);
    } else if (IS("border-top")) {
        border_side(L, 0, t, n);
    } else if (IS("border-right")) {
        border_side(L, 1, t, n);
    } else if (IS("border-bottom")) {
        border_side(L, 2, t, n);
    } else if (IS("border-left")) {
        border_side(L, 3, t, n);
    } else if (IS("background")) {
        struct css_value c = { V_COLOR, 0, 0 };
        for (int i = 0; i < n; i++)
            if (value_of(&t[i], &v, true) && v.type == V_COLOR)
                c = v;
        add(L, P_BACKGROUND_COLOR, c);
    } else if (IS("font")) {
        font_shorthand(L, t, n);
    } else if (IS("font-weight")) {
        weight(L, &t[0]);
    } else if (IS("font-family")) {
        add(L, P_FONT_FAMILY, kwv(family_kind(val, vl)));
    } else if (IS("list-style")) {
        for (int i = 0; i < n; i++) {
            int k = kw_lookup(t[i].s, t[i].len);
            if (k == K_INSIDE || k == K_OUTSIDE)
                add(L, P_LIST_STYLE_POSITION, kwv(k));
            else if (k)
                add(L, P_LIST_STYLE_TYPE, kwv(k));
        }
    } else if (IS("text-decoration") || IS("text-decoration-line")) {
        int deco = 0;
        bool none = false;
        for (int i = 0; i < n; i++) {
            int k = kw_lookup(t[i].s, t[i].len);
            deco |= k == K_UNDERLINE ? DECO_UNDERLINE : k == K_LINE_THROUGH ? DECO_STRIKE
                  : k == K_OVERLINE ? DECO_OVERLINE : 0;
            none |= k == K_NONE_;
        }
        if (deco || none) {
            struct css_value d = { V_NUMBER, 0, deco };
            add(L, P_TEXT_DECORATION, d);
        }
    } else if (IS("overflow-x") || IS("overflow-y")) {
        if (value_of(&t[0], &v, false))
            add(L, P_OVERFLOW, v);
    } else {
        for (size_t i = 0; i < sizeof(simple_props) / sizeof(simple_props[0]); i++) {
            if ((int)strlen(simple_props[i].name) != nl || strncasecmp(simple_props[i].name, name, (size_t)nl))
                continue;
            if (value_of(&t[0], &v, simple_props[i].color))
                add(L, simple_props[i].prop, v);
            break;
        }
    }
#undef IS
}

/* Parses "a: b; c: d !important" into L. */
static void parse_block(struct dlist *L, const char *s, int len)
{
    int i = 0;

    while (i < len) {
        i = skip_ws(s, len, i);
        int ns = i;
        while (i < len && s[i] != ':' && s[i] != ';' && s[i] != '{' && s[i] != '}')
            i++;
        if (i >= len || s[i] != ':') {
            /* Junk or a nested block: skip past it. */
            if (i < len && s[i] == '{')
                i = block_end(s, len, i);
            else
                i++;
            continue;
        }
        int ne = i;
        while (ne > ns && is_ws(s[ne - 1]))
            ne--;
        i++;
        int vs = skip_ws(s, len, i), depth = 0;
        i = vs;
        while (i < len) {
            char c = s[i];
            if (c == '"' || c == '\'') {
                for (i++; i < len && s[i] != c; i++)
                    if (s[i] == '\\')
                        i++;
            } else if (c == '(') {
                depth++;
            } else if (c == ')') {
                depth--;
            } else if ((c == ';' || c == '}') && depth <= 0) {
                break;
            }
            i++;
        }
        int ve = i;
        while (ve > vs && is_ws(s[ve - 1]))
            ve--;
        L->important = false;
        for (int k = ve - 1; k >= vs; k--) {
            if (s[k] == '!') {
                int w = skip_ws(s, ve, k + 1);
                if (ve - w == 9 && !strncasecmp(s + w, "important", 9)) {
                    L->important = true;
                    ve = k;
                    while (ve > vs && is_ws(s[ve - 1]))
                        ve--;
                }
                break;
            }
            if (!isalpha((unsigned char)s[k]) && !is_ws(s[k]))
                break;
        }
        if (ne > ns && ve > vs)
            declaration(L, s + ns, ne - ns, s + vs, ve - vs);
        if (i < len)
            i++;
    }
}

int css_parse_decls(struct arena *a, const char *src, int len, struct css_decl **out)
{
    struct dlist L;

    L.n = 0;
    parse_block(&L, src, len);
    *out = NULL;
    if (!L.n)
        return 0;
    *out = arena_alloc(a, sizeof(struct css_decl) * (size_t)L.n);
    if (!*out)
        return 0;
    memcpy(*out, L.d, sizeof(struct css_decl) * (size_t)L.n);
    return L.n;
}

/* ---- selectors -------------------------------------------------------------- */

static bool ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '-' || c == '_' || (unsigned char)c >= 0x80 || c == '\\';
}

static char *ident(struct cp *C, const char *s, int len, int *i)
{
    int st = *i;

    while (*i < len && ident_char(s[*i])) {
        if (s[*i] == '\\' && *i + 1 < len)
            (*i)++;
        (*i)++;
    }
    if (*i == st)
        return NULL;
    /* Drop backslash escapes ("\:" in utility class names). */
    char *out = arena_alloc(C->a, (size_t)(*i - st) + 1);
    if (!out)
        return NULL;
    int o = 0;
    for (int k = st; k < *i; k++) {
        if (s[k] == '\\' && k + 1 < *i)
            k++;
        out[o++] = s[k];
    }
    out[o] = 0;
    return out;
}

/* One compound selector from s[*i]; false if unsupported or invalid. */
static bool compound(struct cp *C, const char *s, int len, int *i, struct css_compound *c, uint32_t *spec)
{
    const char *cls[16];
    struct css_attrsel attrs[8];
    int any = 0;

    memset(c, 0, sizeof(*c));
    if (*i < len && s[*i] == '*') {
        (*i)++;
        any++;
    } else if (*i < len && (isalpha((unsigned char)s[*i]) || s[*i] == '_')) {
        int st = *i;
        char *name = ident(C, s, len, i);
        if (!name)
            return false;
        for (char *q = name; *q; q++)
            *q = (char)tolower((unsigned char)*q);
        c->tag = (uint16_t)tag_lookup(s + st, *i - st);
        if (!c->tag)
            c->tagname = name;
        *spec += 1;
        any++;
    }
    while (*i < len) {
        char ch = s[*i];
        if (ch == '#') {
            (*i)++;
            if (!(c->id = ident(C, s, len, i)))
                return false;
            *spec += 1 << 16;
        } else if (ch == '.') {
            (*i)++;
            char *name = ident(C, s, len, i);
            if (!name)
                return false;
            if (c->ncls < 16)
                cls[c->ncls++] = name;
            *spec += 1 << 8;
        } else if (ch == '[') {
            int close = *i;
            while (close < len && s[close] != ']')
                close++;
            if (close >= len)
                return false;
            int k = skip_ws(s, close, *i + 1);
            char *name = ident(C, s, close, &k);
            if (!name)
                return false;
            for (char *q = name; *q; q++)
                *q = (char)tolower((unsigned char)*q);
            struct css_attrsel a = { name, NULL, AO_EXISTS };
            k = skip_ws(s, close, k);
            if (k < close) {
                char op = s[k];
                a.op = op == '=' ? AO_EQ : op == '~' ? AO_WORD : op == '^' ? AO_PREFIX : op == '$' ? AO_SUFFIX
                     : op == '*' ? AO_SUBSTR : op == '|' ? AO_DASH : 0xFF;
                if (a.op == 0xFF)
                    return false;
                k += op == '=' ? 1 : 2;
                k = skip_ws(s, close, k);
                int vs = k, ve;
                if (k < close && (s[k] == '"' || s[k] == '\'')) {
                    char q = s[k];
                    vs = ++k;
                    while (k < close && s[k] != q)
                        k++;
                    ve = k;
                } else {
                    while (k < close && !is_ws(s[k]))
                        k++;
                    ve = k;
                }
                a.value = arena_strndup(C->a, s + vs, ve - vs);
                if (!a.value)
                    return false;
            }
            if (c->nattr < 8)
                attrs[c->nattr++] = a;
            *i = close + 1;
            *spec += 1 << 8;
        } else if (ch == ':') {
            (*i)++;
            bool element = *i < len && s[*i] == ':';
            if (element)
                (*i)++;
            int st = *i;
            while (*i < len && ident_char(s[*i]))
                (*i)++;
            int nl = *i - st;
            const char *nm = s + st;
            if (*i < len && s[*i] == '(') {
                int depth = 0;
                for (; *i < len; (*i)++) {
                    if (s[*i] == '(')
                        depth++;
                    else if (s[*i] == ')' && --depth == 0) {
                        (*i)++;
                        break;
                    }
                }
            }
#define PIS(w) ((int)sizeof(w) - 1 == nl && !strncasecmp(nm, w, (size_t)nl))
            if (element || PIS("before") || PIS("after") || PIS("first-line") || PIS("first-letter")) {
                c->pseudo |= PS_NEVER;
            } else if (PIS("link") || PIS("any-link")) {
                c->pseudo |= PS_LINK;
                *spec += 1 << 8;
            } else if (PIS("first-child")) {
                c->pseudo |= PS_FIRST_CHILD;
                *spec += 1 << 8;
            } else if (PIS("last-child")) {
                c->pseudo |= PS_LAST_CHILD;
                *spec += 1 << 8;
            } else if (PIS("only-child")) {
                c->pseudo |= PS_FIRST_CHILD | PS_LAST_CHILD;
                *spec += 1 << 8;
            } else if (PIS("root")) {
                c->pseudo |= PS_ROOT;
                *spec += 1 << 8;
            } else if (PIS("not") || PIS("lang") || PIS("dir")) {
                /* Treated as always true. */
            } else {
                c->pseudo |= PS_NEVER;      /* :hover, :visited, :nth-child, ... */
            }
#undef PIS
        } else {
            break;
        }
        any++;
    }
    if (!any)
        return false;
    if (c->ncls) {
        c->cls = arena_alloc(C->a, sizeof(char *) * c->ncls);
        if (!c->cls)
            return false;
        memcpy(c->cls, cls, sizeof(char *) * c->ncls);
    }
    if (c->nattr) {
        c->attrs = arena_alloc(C->a, sizeof(struct css_attrsel) * c->nattr);
        if (!c->attrs)
            return false;
        memcpy(c->attrs, attrs, sizeof(struct css_attrsel) * c->nattr);
    }
    return true;
}

static bool selector(struct cp *C, const char *s, int len, struct css_selector *out)
{
    struct css_compound parts[16];
    int n = 0, i = skip_ws(s, len, 0);
    uint32_t spec = 0;
    uint8_t comb = SC_NONE;

    while (i < len) {
        if (n >= 16)
            return false;
        if (!compound(C, s, len, &i, &parts[n], &spec))
            return false;
        parts[n].comb = comb;
        n++;
        int j = skip_ws(s, len, i);
        if (j >= len)
            break;
        comb = SC_DESC;
        if (s[j] == '>' || s[j] == '+' || s[j] == '~') {
            comb = s[j] == '>' ? SC_CHILD : s[j] == '+' ? SC_ADJ : SC_SIB;
            j = skip_ws(s, len, j + 1);
        } else if (j == i) {
            return false;           /* junk right after a compound */
        }
        i = j;
    }
    if (!n)
        return false;
    out->parts = arena_alloc(C->a, sizeof(struct css_compound) * (size_t)n);
    if (!out->parts)
        return false;
    memcpy(out->parts, parts, sizeof(struct css_compound) * (size_t)n);
    out->nparts = (uint16_t)n;
    out->spec = spec;
    return true;
}

/* ---- stylesheet -------------------------------------------------------------- */

static bool media_feature(const char *s, int len)
{
    const char *colon = memchr(s, ':', (size_t)len);
    int nl = colon ? (int)(colon - s) : len;
    while (nl && is_ws(s[nl - 1]))
        nl--;
    const char *v = colon ? colon + 1 : s + len;
    int vl = (int)(s + len - v);
    while (vl && is_ws(*v)) {
        v++;
        vl--;
    }
    while (vl && is_ws(v[vl - 1]))
        vl--;
#define FIS(w) ((int)sizeof(w) - 1 == nl && !strncasecmp(s, w, (size_t)nl))
#define VIS(w) ((int)sizeof(w) - 1 == vl && !strncasecmp(v, w, (size_t)vl))
    struct css_value px;
    int width = 0;
    if (parse_length(v, vl, &px)) {
        width = px.num / 64;
        if (px.type == V_LENGTH && (px.unit == U_EM || px.unit == U_REM))
            width *= 16;
    }
    if (FIS("max-width") || FIS("max-device-width"))
        return WEB_VIEW_CSS_W <= width;
    if (FIS("min-width") || FIS("min-device-width"))
        return WEB_VIEW_CSS_W >= width;
    if (FIS("orientation"))
        return VIS("portrait");
    if (FIS("prefers-color-scheme"))
        return VIS("light");
    if (FIS("prefers-reduced-motion"))
        return VIS("no-preference");
    if (FIS("hover") || FIS("any-hover"))
        return VIS("none");
    if (FIS("pointer") || FIS("any-pointer"))
        return VIS("coarse");
    if (FIS("color"))
        return true;
    if (FIS("-webkit-min-device-pixel-ratio") || FIS("min-resolution") || FIS("min--moz-device-pixel-ratio"))
        return width <= 1;
    return false;
#undef FIS
#undef VIS
}

static bool media_matches(const char *s, int len)
{
    int i = 0;

    while (i <= len) {
        int e = i;
        int depth = 0;
        while (e < len && (s[e] != ',' || depth)) {
            depth += s[e] == '(' ? 1 : s[e] == ')' ? -1 : 0;
            e++;
        }
        /* One query: [not|only] [type] [and (feature)]... */
        bool ok = true, neg = false;
        int k = skip_ws(s, e, i);
        while (k < e) {
            if (s[k] == '(') {
                int c = k;
                while (c < e && s[c] != ')')
                    c++;
                ok &= media_feature(s + k + 1, c - k - 1);
                k = c + 1;
            } else {
                int ws = k;
                while (k < e && !is_ws(s[k]) && s[k] != '(')
                    k++;
                int wl = k - ws;
                const char *w = s + ws;
                if (wl == 3 && !strncasecmp(w, "not", 3))
                    neg = true;
                else if ((wl == 4 && !strncasecmp(w, "only", 4)) || (wl == 3 && !strncasecmp(w, "and", 3)))
                    ;
                else if ((wl == 6 && !strncasecmp(w, "screen", 6)) || (wl == 3 && !strncasecmp(w, "all", 3)) ||
                         (wl == 8 && !strncasecmp(w, "handheld", 8)))
                    ;
                else
                    ok = false;     /* print, speech, tv, ... */
            }
            k = skip_ws(s, e, k);
        }
        if (ok != neg)
            return true;
        i = e + 1;
    }
    return false;
}

static void add_rule(struct cp *C, struct stylesheet *ss, const char *sel, int sl, const char *body, int bl)
{
    struct css_selector sels[MAX_SELECTORS];
    int ns = 0, i = 0;

    while (i < sl && ns < MAX_SELECTORS) {
        int e = i, depth = 0;
        while (e < sl && (sel[e] != ',' || depth)) {
            depth += sel[e] == '(' ? 1 : sel[e] == ')' ? -1 : 0;
            e++;
        }
        if (selector(C, sel + i, e - i, &sels[ns]))
            ns++;
        i = e + 1;
    }
    if (!ns)
        return;
    struct dlist *L = malloc(sizeof(*L));
    if (!L)
        return;
    L->n = 0;
    parse_block(L, body, bl);
    if (L->n && ss->nrules < 65535) {
        if (ss->nrules == ss->cap) {
            int cap = ss->cap ? ss->cap * 2 : 64;
            struct css_rule *r = realloc(ss->rules, sizeof(*r) * (size_t)cap);
            if (!r) {
                free(L);
                return;
            }
            ss->rules = r;
            ss->cap = cap;
        }
        struct css_rule *r = &ss->rules[ss->nrules];
        r->sels = arena_alloc(C->a, sizeof(struct css_selector) * (size_t)ns);
        r->decls = arena_alloc(C->a, sizeof(struct css_decl) * (size_t)L->n);
        if (r->sels && r->decls) {
            memcpy(r->sels, sels, sizeof(struct css_selector) * (size_t)ns);
            memcpy(r->decls, L->d, sizeof(struct css_decl) * (size_t)L->n);
            r->nsels = (uint16_t)ns;
            r->ndecls = (uint16_t)L->n;
            ss->nrules++;
        }
    }
    free(L);
}

static void parse_rules(struct cp *C, struct stylesheet *ss, const char *s, int len, int depth)
{
    int i = 0;

    while (i < len) {
        i = skip_ws(s, len, i);
        if (i >= len)
            break;
        if (s[i] == '<' || s[i] == '-') {
            /* "<!--" and "-->" around old inline styles. */
            if (!strncmp(s + i, "<!--", 4) && i + 4 <= len) {
                i += 4;
                continue;
            }
            if (!strncmp(s + i, "-->", 3) && i + 3 <= len) {
                i += 3;
                continue;
            }
        }
        if (s[i] == '}') {
            i++;
            continue;
        }
        if (s[i] == '@') {
            int ns = ++i;
            while (i < len && (isalnum((unsigned char)s[i]) || s[i] == '-'))
                i++;
            int nl = i - ns;
            int ps = i;
            while (i < len && s[i] != '{' && s[i] != ';')
                i++;
            if (i >= len)
                break;
            if (s[i] == ';') {
                i++;
                continue;           /* @import, @charset, @namespace */
            }
            int end = block_end(s, len, i);
            bool media = nl == 5 && !strncasecmp(s + ns, "media", 5);
            bool supports = nl == 8 && !strncasecmp(s + ns, "supports", 8);
            if (depth < 4 && ((media && media_matches(s + ps, i - ps)) || supports))
                parse_rules(C, ss, s + i + 1, end - i - 2 > 0 ? end - i - 2 : 0, depth + 1);
            i = end;
            continue;
        }
        int ss_start = i;
        while (i < len && s[i] != '{') {
            if (s[i] == '"' || s[i] == '\'') {
                char q = s[i];
                for (i++; i < len && s[i] != q; i++)
                    ;
            }
            i++;
        }
        if (i >= len)
            break;
        int end = block_end(s, len, i);
        add_rule(C, ss, s + ss_start, i - ss_start, s + i + 1, end - i - 2 > 0 ? end - i - 2 : 0);
        i = end;
    }
}

static uint32_t hash_cs(const char *s)
{
    uint32_t h = 2166136261u;

    while (*s)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

uint32_t css_key(int kind, uint32_t h)
{
    return (uint32_t)kind << 30 | (h & 0x3FFFFFFFu);
}

uint32_t css_hash(const char *s)
{
    return hash_cs(s);
}

static int index_cmp(const struct css_index_entry *a, const struct css_index_entry *b)
{
    if (a->key != b->key)
        return a->key < b->key ? -1 : 1;
    if (a->rule != b->rule)
        return a->rule < b->rule ? -1 : 1;
    return a->sel < b->sel ? -1 : a->sel > b->sel;
}

/* Bottom-up merge sort (libc's qsort is an insertion sort). */
static void sort_index(struct css_index_entry *e, int n)
{
    struct css_index_entry *tmp = malloc(sizeof(*e) * (size_t)n);

    if (!tmp) {
        for (int i = 1; i < n; i++) {
            struct css_index_entry x = e[i];
            int j = i;
            for (; j > 0 && index_cmp(&e[j - 1], &x) > 0; j--)
                e[j] = e[j - 1];
            e[j] = x;
        }
        return;
    }
    for (int w = 1; w < n; w *= 2) {
        for (int lo = 0; lo < n; lo += 2 * w) {
            int mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            int a = lo, b = mid, o = lo;
            while (a < mid && b < hi)
                tmp[o++] = index_cmp(&e[a], &e[b]) <= 0 ? e[a++] : e[b++];
            while (a < mid)
                tmp[o++] = e[a++];
            while (b < hi)
                tmp[o++] = e[b++];
        }
        memcpy(e, tmp, sizeof(*e) * (size_t)n);
    }
    free(tmp);
}

static void build_index(struct stylesheet *ss)
{
    int n = 0;

    for (int r = 0; r < ss->nrules; r++)
        n += ss->rules[r].nsels;
    free(ss->index);
    ss->index = n ? malloc(sizeof(struct css_index_entry) * (size_t)n) : NULL;
    ss->nindex = 0;
    if (!ss->index)
        return;
    for (int r = 0; r < ss->nrules; r++) {
        for (int s = 0; s < ss->rules[r].nsels; s++) {
            const struct css_selector *sel = &ss->rules[r].sels[s];
            const struct css_compound *c = &sel->parts[sel->nparts - 1];
            struct css_index_entry *e = &ss->index[ss->nindex++];
            if (c->pseudo & PS_NEVER) {
                ss->nindex--;
                continue;
            }
            if (c->id)
                e->key = css_key(3, hash_cs(c->id));
            else if (c->ncls)
                e->key = css_key(2, hash_cs(c->cls[0]));
            else if (c->tag)
                e->key = css_key(1, c->tag);
            else if (c->tagname)
                e->key = css_key(1, hash_cs(c->tagname) | 0x10000);
            else
                e->key = css_key(0, 0);
            e->rule = r;
            e->sel = (uint16_t)s;
        }
    }
    sort_index(ss->index, ss->nindex);
}

void css_parse_sheet(struct arena *a, struct stylesheet *ss, const char *src, int len)
{
    struct cp C = { a, src, len };

    parse_rules(&C, ss, src, len, 0);
    build_index(ss);
    ss->loaded = true;
}

void css_free_sheet(struct stylesheet *ss)
{
    free(ss->rules);
    free(ss->index);
    memset(ss, 0, sizeof(*ss));
}
