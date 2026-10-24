/*
 * HTML parser: a tolerant tokenizer and a simplified tree builder.
 *
 * It follows the parts of the HTML5 algorithm that real pages lean on:
 * void elements, raw text (script, style), RCDATA (title, textarea),
 * implied end tags (p, li, dt/dd, option, table rows and cells), head
 * content before the body, and end tags that only close elements in
 * scope. It doesn't do the adoption agency or foster parenting. The open
 * element stack is capped at HTML_MAX_DEPTH, since layout recurses on it.
 */

#include "web.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- character references ----------------------------------------------- */

static const char *const latin1_names[96] = {
    "nbsp", "iexcl", "cent", "pound", "curren", "yen", "brvbar", "sect", "uml", "copy", "ordf",
    "laquo", "not", "shy", "reg", "macr", "deg", "plusmn", "sup2", "sup3", "acute", "micro",
    "para", "middot", "cedil", "sup1", "ordm", "raquo", "frac14", "frac12", "frac34", "iquest",
    "Agrave", "Aacute", "Acirc", "Atilde", "Auml", "Aring", "AElig", "Ccedil", "Egrave",
    "Eacute", "Ecirc", "Euml", "Igrave", "Iacute", "Icirc", "Iuml", "ETH", "Ntilde", "Ograve",
    "Oacute", "Ocirc", "Otilde", "Ouml", "times", "Oslash", "Ugrave", "Uacute", "Ucirc", "Uuml",
    "Yacute", "THORN", "szlig", "agrave", "aacute", "acirc", "atilde", "auml", "aring", "aelig",
    "ccedil", "egrave", "eacute", "ecirc", "euml", "igrave", "iacute", "icirc", "iuml", "eth",
    "ntilde", "ograve", "oacute", "ocirc", "otilde", "ouml", "divide", "oslash", "ugrave",
    "uacute", "ucirc", "uuml", "yacute", "thorn", "yuml",
};

static const struct {
    const char *name;
    uint32_t cp;
} entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
    { "hellip", 0x2026 }, { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "lsquo", 0x2018 },
    { "rsquo", 0x2019 }, { "sbquo", 0x201A }, { "ldquo", 0x201C }, { "rdquo", 0x201D },
    { "bdquo", 0x201E }, { "lsaquo", 0x2039 }, { "rsaquo", 0x203A }, { "bull", 0x2022 },
    { "trade", 0x2122 }, { "euro", 0x20AC }, { "larr", 0x2190 }, { "rarr", 0x2192 },
    { "uarr", '^' }, { "darr", 'v' }, { "harr", 0x2194 }, { "ensp", 0x2002 },
    { "emsp", 0x2003 }, { "thinsp", 0x2009 }, { "zwnj", 0x200C }, { "zwj", 0x200D },
    { "lrm", 0x200E }, { "rlm", 0x200F }, { "minus", 0x2212 }, { "le", 0x2264 },
    { "ge", 0x2265 }, { "ne", 0x2260 }, { "dagger", 0x2020 }, { "Dagger", '+' },
    { "permil", 0x2030 }, { "OElig", 0x152 }, { "oelig", 0x153 }, { "Scaron", 0x160 },
    { "scaron", 0x161 }, { "Yuml", 0x178 }, { "fnof", 0x192 }, { "circ", 0x2C6 },
    { "tilde", 0x2DC }, { "prime", 0x2032 }, { "Prime", 0x2033 }, { "frasl", 0x2044 },
    { "lowast", 0x2217 }, { "sim", 0x223C }, { "check", 0x2713 }, { "nbsp", 0xA0 },
    { "colon", ':' }, { "comma", ',' }, { "period", '.' }, { "excl", '!' }, { "quest", '?' },
    { "num", '#' }, { "dollar", '$' }, { "percnt", '%' }, { "lpar", '(' }, { "rpar", ')' },
    { "ast", '*' }, { "plus", '+' }, { "sol", '/' }, { "semi", ';' }, { "equals", '=' },
    { "lsqb", '[' }, { "rsqb", ']' }, { "lowbar", '_' }, { "grave", '`' }, { "lcub", '{' },
    { "rcub", '}' }, { "verbar", '|' }, { "Hat", '^' }, { "NewLine", '\n' }, { "Tab", '\t' },
    { "middot", 0xB7 }, { "squ", 0x25AA }, { "rarrow", 0x2192 },
};

/* References that also work without the ';' (the common legacy ones). */
static bool legacy_ok(const char *name, int len)
{
    static const char *const legacy[] = { "amp", "lt", "gt", "quot", "nbsp", "copy", "reg" };

    for (size_t i = 0; i < sizeof(legacy) / sizeof(legacy[0]); i++)
        if ((int)strlen(legacy[i]) == len && !strncmp(name, legacy[i], (size_t)len))
            return true;
    return false;
}

static int lookup_entity(const char *name, int len)
{
    for (int i = 0; i < 96; i++)
        if ((int)strlen(latin1_names[i]) == len && !strncmp(latin1_names[i], name, (size_t)len))
            return 0xA0 + i;
    for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); i++)
        if ((int)strlen(entities[i].name) == len && !strncmp(entities[i].name, name, (size_t)len))
            return (int)entities[i].cp;
    return -1;
}

/* Windows-1252 for numeric references to C1 controls (HTML5). */
static uint32_t c1_fix(uint32_t cp)
{
    static const uint16_t map[32] = {
        0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013,
        0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178,
    };

    return cp >= 0x80 && cp < 0xA0 ? map[cp - 0x80] : cp;
}

int html_unescape(const char *s, int n, char *out, bool in_attr)
{
    int o = 0;

    for (int i = 0; i < n;) {
        if (s[i] != '&' || i + 1 >= n) {
            out[o++] = s[i++];
            continue;
        }
        int j = i + 1;
        if (s[j] == '#') {
            j++;
            bool hex = j < n && (s[j] == 'x' || s[j] == 'X');
            if (hex)
                j++;
            uint32_t cp = 0;
            int digits = 0;
            while (j < n && (hex ? isxdigit((unsigned char)s[j]) : isdigit((unsigned char)s[j]))) {
                int d = isdigit((unsigned char)s[j]) ? s[j] - '0' : (tolower((unsigned char)s[j]) - 'a' + 10);
                if (cp < 0x110000)
                    cp = cp * (hex ? 16 : 10) + (uint32_t)d;
                j++;
                digits++;
            }
            if (!digits) {
                out[o++] = s[i++];
                continue;
            }
            if (j < n && s[j] == ';')
                j++;
            if (!cp || cp >= 0x110000 || (cp >= 0xD800 && cp < 0xE000))
                cp = 0xFFFD;
            char tmp[8];
            int k = cs_put(c1_fix(cp), tmp);
            if (k > j - i)
                k = j - i;          /* never longer than the reference */
            memcpy(out + o, tmp, (size_t)k);
            o += k;
            i = j;
            continue;
        }
        while (j < n && j - i <= 32 && isalnum((unsigned char)s[j]))
            j++;
        int len = j - i - 1;
        bool semi = j < n && s[j] == ';';
        int cp = len > 0 ? lookup_entity(s + i + 1, len) : -1;
        if (cp >= 0 && !semi) {
            /* "&copy=1" in a URL is not a reference. */
            if (!legacy_ok(s + i + 1, len) || (in_attr && j < n && (isalnum((unsigned char)s[j]) || s[j] == '=')))
                cp = -1;
        }
        if (cp < 0) {
            out[o++] = s[i++];
            continue;
        }
        char tmp[8];
        int k = cs_put((uint32_t)cp, tmp);
        if (k > j - i + semi)
            k = j - i + semi;
        memcpy(out + o, tmp, (size_t)k);
        o += k;
        i = j + semi;
    }
    return o;
}

/* ---- tree building ---------------------------------------------------------- */

struct parser {
    struct arena *a;
    const char *s;
    int len, pos;
    struct node *doc, *html, *head, *body;
    struct node *stack[HTML_MAX_DEPTH];
    int depth;
    int nodes;
    bool in_body, full, scripting_enabled;
};

static bool is_void(int tag)
{
    switch (tag) {
    case T_AREA: case T_BASE: case T_BR: case T_COL: case T_EMBED: case T_HR: case T_IMG:
    case T_INPUT: case T_LINK: case T_META: case T_PARAM: case T_SOURCE: case T_TRACK:
    case T_WBR: case T_FRAME:
        return true;
    }
    return false;
}

static bool is_head_tag(int tag)
{
    return tag == T_TITLE || tag == T_META || tag == T_LINK || tag == T_STYLE || tag == T_SCRIPT ||
           tag == T_BASE || tag == T_NOSCRIPT || tag == T_TEMPLATE;
}

/* Start tags that close an open <p>. */
static bool closes_p(int tag)
{
    switch (tag) {
    case T_ADDRESS: case T_ARTICLE: case T_ASIDE: case T_BLOCKQUOTE: case T_CENTER: case T_DETAILS:
    case T_DIALOG: case T_DIR: case T_DIV: case T_DL: case T_FIELDSET: case T_FIGCAPTION:
    case T_FIGURE: case T_FOOTER: case T_FORM: case T_H1: case T_H2: case T_H3: case T_H4:
    case T_H5: case T_H6: case T_HEADER: case T_HGROUP: case T_HR: case T_LI: case T_DD: case T_DT:
    case T_MAIN: case T_MENU: case T_NAV: case T_OL: case T_P: case T_PRE: case T_SECTION:
    case T_SUMMARY: case T_TABLE: case T_UL: case T_XMP: case T_LISTING: case T_PLAINTEXT:
        return true;
    }
    return false;
}

static bool is_heading(int tag)
{
    return tag >= T_H1 && tag <= T_H6;
}

static bool scope_boundary(int tag)
{
    return tag == T_TABLE || tag == T_TD || tag == T_TH || tag == T_CAPTION || tag == T_HTML ||
           tag == T_OBJECT || tag == T_TEMPLATE || tag == T_BUTTON;
}

static struct node *cur(struct parser *P)
{
    return P->stack[P->depth - 1];
}

static struct node *new_node(struct parser *P, int type)
{
    if (P->nodes >= HTML_MAX_NODES) {
        P->full = true;
        return NULL;
    }
    struct node *n = arena_alloc(P->a, sizeof(*n));
    if (!n) {
        P->full = true;
        return NULL;
    }
    n->type = (uint8_t)type;
    n->img = -1;
    n->frame = -1;
    n->nsdecl = -1;
    P->nodes++;
    return n;
}

static void append(struct node *parent, struct node *child)
{
    child->parent = parent;
    child->prev = parent->last;
    if (parent->last)
        parent->last->next = child;
    else
        parent->first = child;
    parent->last = child;
}

static struct node *new_elem(struct parser *P, int tag, const char *name)
{
    struct node *n = new_node(P, N_ELEM);

    if (n) {
        n->tag = (uint16_t)tag;
        n->name = name ? name : tag_name(tag);
    }
    return n;
}

/* Index of the nearest open element with this tag, searching down to a
 * boundary (or -1). */
static int find_open(struct parser *P, int tag, const char *name, bool (*boundary)(int))
{
    for (int i = P->depth - 1; i >= 2; i--) {
        struct node *n = P->stack[i];
        if (n->tag == tag && (tag != T_UNKNOWN || !strcmp(n->name, name)))
            return i;
        if (boundary && boundary(n->tag))
            return -1;
    }
    return -1;
}

static void pop_to(struct parser *P, int index)
{
    if (index >= 2)
        P->depth = index;
}

static bool p_scope(int tag)
{
    return scope_boundary(tag);
}

static bool list_scope(int tag)
{
    return scope_boundary(tag) || tag == T_UL || tag == T_OL;
}

static bool dl_scope(int tag)
{
    return scope_boundary(tag) || tag == T_DL;
}

static bool table_scope(int tag)
{
    return tag == T_TABLE || tag == T_HTML || tag == T_TEMPLATE;
}

static void close_p(struct parser *P)
{
    int i = find_open(P, T_P, NULL, p_scope);

    if (i >= 0)
        pop_to(P, i);
}

static void enter_body(struct parser *P)
{
    if (!P->in_body) {
        P->in_body = true;
        P->depth = 2;               /* html, body */
    }
}

static void push(struct parser *P, struct node *n)
{
    append(cur(P), n);
    if (P->depth < HTML_MAX_DEPTH)
        P->stack[P->depth++] = n;
}

/* Closing implied by a start tag (before it is inserted). */
static void implied_end(struct parser *P, int tag)
{
    int i;

    if (closes_p(tag))
        close_p(P);
    switch (tag) {
    case T_LI:
        if ((i = find_open(P, T_LI, NULL, list_scope)) >= 0)
            pop_to(P, i);
        break;
    case T_DT:
    case T_DD:
        if ((i = find_open(P, T_DT, NULL, dl_scope)) >= 0)
            pop_to(P, i);
        if ((i = find_open(P, T_DD, NULL, dl_scope)) >= 0)
            pop_to(P, i);
        break;
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
        if (is_heading(cur(P)->tag))
            P->depth--;
        break;
    case T_A:
        if ((i = find_open(P, T_A, NULL, scope_boundary)) >= 0)
            pop_to(P, i);
        break;
    case T_NOBR:
        if ((i = find_open(P, T_NOBR, NULL, scope_boundary)) >= 0)
            pop_to(P, i);
        break;
    case T_BUTTON:
        if ((i = find_open(P, T_BUTTON, NULL, scope_boundary)) >= 0)
            pop_to(P, i);
        break;
    case T_OPTION:
        if (cur(P)->tag == T_OPTION)
            P->depth--;
        break;
    case T_OPTGROUP:
        if (cur(P)->tag == T_OPTION)
            P->depth--;
        if (cur(P)->tag == T_OPTGROUP)
            P->depth--;
        break;
    case T_SELECT:
        if ((i = find_open(P, T_SELECT, NULL, scope_boundary)) >= 0)
            pop_to(P, i);
        break;
    case T_THEAD: case T_TBODY: case T_TFOOT: case T_CAPTION: case T_COLGROUP:
        if ((i = find_open(P, T_TABLE, NULL, table_scope)) >= 0)
            pop_to(P, i + 1);
        break;
    case T_TR:
        if ((i = find_open(P, T_TABLE, NULL, table_scope)) >= 0) {
            /* Close cells and rows, keep a row group. */
            while (P->depth > i + 1) {
                int t = cur(P)->tag;
                if (t == T_THEAD || t == T_TBODY || t == T_TFOOT)
                    break;
                P->depth--;
            }
        }
        break;
    case T_TD:
    case T_TH:
        if ((i = find_open(P, T_TABLE, NULL, table_scope)) >= 0) {
            while (P->depth > i + 1 && cur(P)->tag != T_TR) {
                int t = cur(P)->tag;
                if (t == T_THEAD || t == T_TBODY || t == T_TFOOT)
                    break;
                P->depth--;
            }
            if (cur(P)->tag != T_TR) {
                struct node *tr = new_elem(P, T_TR, NULL);
                if (tr)
                    push(P, tr);
            }
        }
        break;
    }
}

/* Raw text up to "</name", case-insensitively; returns its end. */
static int raw_end(struct parser *P, const char *name)
{
    int nl = (int)strlen(name);

    for (int i = P->pos; i + 2 + nl <= P->len; i++) {
        if (P->s[i] == '<' && P->s[i + 1] == '/' && !strncasecmp(P->s + i + 2, name, (size_t)nl)) {
            char c = i + 2 + nl < P->len ? P->s[i + 2 + nl] : '>';
            if (c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/' || c == '\f')
                return i;
        }
    }
    return P->len;
}

static void skip_past(struct parser *P, char ch)
{
    while (P->pos < P->len && P->s[P->pos] != ch)
        P->pos++;
    if (P->pos < P->len)
        P->pos++;
}

static void add_text(struct parser *P, struct node *parent, const char *s, int n, bool decode)
{
    if (n <= 0 || !parent)
        return;
    struct node *last = parent->last;
    char *buf;
    int len;
    if (last && last->type == N_TEXT) {
        /* Merge with the previous text (split by a comment, say). */
        buf = arena_alloc(P->a, (size_t)(last->len + n + 1));
        if (!buf) {
            P->full = true;
            return;
        }
        memcpy(buf, last->text, (size_t)last->len);
        len = last->len + (decode ? html_unescape(s, n, buf + last->len, false) : n);
        if (!decode)
            memcpy(buf + last->len, s, (size_t)n);
        buf[len] = 0;
        last->text = buf;
        last->len = len;
        return;
    }
    struct node *t = new_node(P, N_TEXT);
    if (!t)
        return;
    buf = arena_alloc(P->a, (size_t)n + 1);
    if (!buf) {
        P->full = true;
        return;
    }
    len = decode ? html_unescape(s, n, buf, false) : n;
    if (!decode)
        memcpy(buf, s, (size_t)n);
    buf[len] = 0;
    t->text = buf;
    t->len = len;
    append(parent, t);
}

static bool all_space(const char *s, int n)
{
    for (int i = 0; i < n; i++)
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r' && s[i] != '\f')
            return false;
    return true;
}

static void text(struct parser *P, const char *s, int n)
{
    if (!P->in_body) {
        if (all_space(s, n))
            return;
        enter_body(P);
    }
    int t = cur(P)->tag;
    if ((t == T_TABLE || t == T_TBODY || t == T_THEAD || t == T_TFOOT || t == T_TR ||
         t == T_SELECT || t == T_UL || t == T_OL || t == T_DL) && all_space(s, n))
        return;
    add_text(P, cur(P), s, n, true);
}

static void merge_attrs(struct node *dst, struct attr *src)
{
    for (struct attr *a = src, *next; a; a = next) {
        next = a->next;
        if (!node_attr(dst, a->name)) {
            a->next = dst->attrs;
            dst->attrs = a;
        }
    }
}

static struct attr *parse_attrs(struct parser *P, bool *self_close)
{
    struct attr *head = NULL, **tail = &head;
    int count = 0;

    *self_close = false;
    for (;;) {
        while (P->pos < P->len && (isspace((unsigned char)P->s[P->pos]) || P->s[P->pos] == '/')) {
            if (P->s[P->pos] == '/' && P->pos + 1 < P->len && P->s[P->pos + 1] == '>')
                *self_close = true;
            P->pos++;
        }
        if (P->pos >= P->len || P->s[P->pos] == '>') {
            P->pos++;
            return head;
        }
        int ns = P->pos;
        while (P->pos < P->len && !isspace((unsigned char)P->s[P->pos]) && P->s[P->pos] != '=' &&
               P->s[P->pos] != '>' && (P->s[P->pos] != '/' || P->pos == ns))
            P->pos++;
        int nl = P->pos - ns;
        while (P->pos < P->len && isspace((unsigned char)P->s[P->pos]))
            P->pos++;
        const char *vs = "";
        int vl = 0;
        if (P->pos < P->len && P->s[P->pos] == '=') {
            P->pos++;
            while (P->pos < P->len && isspace((unsigned char)P->s[P->pos]))
                P->pos++;
            if (P->pos < P->len && (P->s[P->pos] == '"' || P->s[P->pos] == '\'')) {
                char q = P->s[P->pos++];
                vs = P->s + P->pos;
                while (P->pos < P->len && P->s[P->pos] != q)
                    P->pos++;
                vl = (int)(P->s + P->pos - vs);
                if (P->pos < P->len)
                    P->pos++;
            } else {
                vs = P->s + P->pos;
                while (P->pos < P->len && !isspace((unsigned char)P->s[P->pos]) && P->s[P->pos] != '>')
                    P->pos++;
                vl = (int)(P->s + P->pos - vs);
            }
        }
        if (nl <= 0 || count >= 32)
            continue;
        char *name = arena_strndup(P->a, P->s + ns, nl);
        char *value = arena_alloc(P->a, (size_t)vl + 1);
        struct attr *a = arena_alloc(P->a, sizeof(*a));
        if (!name || !value || !a) {
            P->full = true;
            return head;
        }
        for (char *c = name; *c; c++)
            *c = (char)tolower((unsigned char)*c);
        bool dup = false;
        for (struct attr *o = head; o; o = o->next)
            dup |= !strcmp(o->name, name);
        if (dup)
            continue;
        value[html_unescape(vs, vl, value, true)] = 0;
        a->name = name;
        a->value = value;
        *tail = a;
        tail = &a->next;
        count++;
    }
}

static void start_tag(struct parser *P, int tag, const char *name, struct attr *attrs, bool self_close)
{
    if (tag == T_HTML) {
        merge_attrs(P->html, attrs);
        return;
    }
    if (tag == T_BODY) {
        merge_attrs(P->body, attrs);
        enter_body(P);
        return;
    }
    if (tag == T_HEAD)
        return;

    struct node *parent;
    if (!P->in_body && is_head_tag(tag)) {
        parent = P->head;
    } else {
        enter_body(P);
        implied_end(P, tag);
        parent = cur(P);
    }
    struct node *n = new_elem(P, tag, name);
    if (!n)
        return;
    n->attrs = attrs;

    /* Raw text and RCDATA content. */
    const char *raw = NULL;
    bool rcdata = false;
    switch (tag) {
    case T_SCRIPT: raw = "script"; break;
    case T_STYLE: raw = "style"; break;
    case T_XMP: raw = "xmp"; break;
    case T_IFRAME: raw = "iframe"; break;
    case T_NOSCRIPT: if (P->scripting_enabled) raw = "noscript"; break;
    default: break;
    }
    if (tag == T_TITLE) {
        raw = "title";
        rcdata = true;
    } else if (tag == T_TEXTAREA) {
        raw = "textarea";
        rcdata = true;
    } else if (tag == T_PLAINTEXT) {
        append(parent, n);
        add_text(P, n, P->s + P->pos, P->len - P->pos, false);
        P->pos = P->len;
        return;
    }
    /* HTML's self-closing slash cannot activate a noscript fallback. */
    if (raw && (!self_close || tag == T_NOSCRIPT)) {
        append(parent, n);
        int end = raw_end(P, raw);
        int start = P->pos;
        if (tag == T_TEXTAREA && start < end && P->s[start] == '\n')
            start++;
        if (tag != T_IFRAME)
            add_text(P, n, P->s + start, end - start, rcdata);
        P->pos = end;
        if (P->pos < P->len)
            skip_past(P, '>');
        return;
    }
    if (parent == P->head || is_void(tag) || self_close) {
        append(parent, n);
        return;
    }
    push(P, n);
    if ((tag == T_PRE || tag == T_LISTING) && P->pos < P->len && P->s[P->pos] == '\n')
        P->pos++;                   /* a newline right after <pre> is dropped */
}

static void end_tag(struct parser *P, int tag, const char *name)
{
    int i;

    if (tag == T_HTML || tag == T_BODY || tag == T_HEAD)
        return;
    if (tag == T_BR) {
        start_tag(P, T_BR, NULL, NULL, true);
        return;
    }
    if (!P->in_body || is_void(tag))
        return;
    switch (tag) {
    case T_TABLE:
        i = find_open(P, T_TABLE, NULL, NULL);
        break;
    case T_TR: case T_TBODY: case T_THEAD: case T_TFOOT: case T_TD: case T_TH: case T_CAPTION:
        i = find_open(P, tag, NULL, table_scope);
        break;
    case T_LI:
        i = find_open(P, tag, NULL, list_scope);
        break;
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
        /* Any heading closes any heading. */
        i = -1;
        for (int k = P->depth - 1; k >= 2 && !scope_boundary(P->stack[k]->tag); k--) {
            if (is_heading(P->stack[k]->tag)) {
                i = k;
                break;
            }
        }
        break;
    default:
        i = find_open(P, tag, name, scope_boundary);
        break;
    }
    if (i >= 0)
        pop_to(P, i);
}

struct node *html_parse(struct arena *a, const char *src, int len, bool scripting_enabled)
{
    struct parser *P = arena_alloc(a, sizeof(*P));

    if (!P)
        return NULL;
    P->a = a;
    P->s = src;
    P->len = len;
    P->scripting_enabled = scripting_enabled;
    P->doc = new_node(P, N_DOC);
    P->html = new_elem(P, T_HTML, NULL);
    P->head = new_elem(P, T_HEAD, NULL);
    P->body = new_elem(P, T_BODY, NULL);
    if (!P->doc || !P->html || !P->head || !P->body)
        return NULL;
    append(P->doc, P->html);
    append(P->html, P->head);
    append(P->html, P->body);
    P->stack[0] = P->html;
    P->stack[1] = P->body;
    P->depth = 2;

    while (P->pos < P->len && !P->full) {
        const char *s = P->s;
        int start = P->pos;
        if (s[start] != '<') {
            const char *lt = memchr(s + start, '<', (size_t)(P->len - start));
            int end = lt ? (int)(lt - s) : P->len;
            text(P, s + start, end - start);
            P->pos = end;
            continue;
        }
        if (start + 1 >= P->len) {
            text(P, s + start, 1);
            P->pos++;
            continue;
        }
        char c = s[start + 1];
        if (c == '!') {
            if (start + 4 <= P->len && !strncmp(s + start, "<!--", 4)) {
                const char *e = NULL;
                for (int i = start + 4; i + 3 <= P->len; i++) {
                    if (s[i] == '-' && s[i + 1] == '-' && s[i + 2] == '>') {
                        e = s + i + 3;
                        break;
                    }
                }
                P->pos = e ? (int)(e - s) : P->len;
            } else {
                P->pos = start + 2;
                skip_past(P, '>');
            }
            continue;
        }
        if (c == '?') {
            P->pos = start + 2;
            skip_past(P, '>');
            continue;
        }
        bool end = c == '/';
        int ns = start + 1 + end;
        if (ns >= P->len || !isalpha((unsigned char)s[ns])) {
            if (end) {
                P->pos = ns;
                skip_past(P, '>');  /* "</ junk>" is a bogus comment */
            } else {
                text(P, s + start, 1);
                P->pos++;
            }
            continue;
        }
        int ne = ns;
        while (ne < P->len && !isspace((unsigned char)s[ne]) && s[ne] != '>' && s[ne] != '/')
            ne++;
        int tag = tag_lookup(s + ns, ne - ns);
        const char *name = NULL;
        if (!tag) {
            char *nm = arena_strndup(a, s + ns, ne - ns);
            if (!nm)
                break;
            for (char *q = nm; *q; q++)
                *q = (char)tolower((unsigned char)*q);
            name = nm;
        }
        P->pos = ne;
        bool self_close;
        struct attr *attrs = parse_attrs(P, &self_close);
        if (end)
            end_tag(P, tag, name);
        else
            start_tag(P, tag, name, attrs, self_close);
    }
    return P->doc;
}
