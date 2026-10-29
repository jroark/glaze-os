/*
 * The browser engine: DOM, CSS, style, layout and painting.
 *
 * Text inside the engine is Latin-1 bytes plus libgfx's extra glyphs at
 * 0x7f-0x87 (charset.c maps other code points onto them or onto ASCII), so
 * strings go straight to gfx_text(). CSS lengths are integers: values are
 * parsed as 1/64ths and resolved to device pixels at 3/4 of a CSS pixel,
 * which makes the 240 px screen 320 CSS px wide. Nothing here touches
 * syscalls or libui, so the engine also builds on the host (host/Makefile).
 */
#ifndef BROWSER_WEB_H
#define BROWSER_WEB_H

#include <glaze/gfx.h>
#include <glaze/res.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEB_VIEW_CSS_W  320         /* viewport width in CSS px */
#define CSS_TO_DEV(v)   ((v) * 3 / 4)

/* ---- errors (negative results of the network and loader layers) ---------- */

enum {
    WE_OK = 0,
    WE_NETDOWN = -100,      /* no network */
    WE_DNS,                 /* name lookup failed */
    WE_CONNECT,             /* refused or unreachable */
    WE_TIMEOUT,
    WE_RESET,               /* connection dropped mid-response */
    WE_PROTOCOL,            /* malformed HTTP */
    WE_TOO_BIG,
    WE_TLS,                 /* TLS handshake or record failure */
    WE_CERT,                /* certificate not trusted */
    WE_CERT_NAME,           /* certificate for another host */
    WE_CERT_DATE,           /* certificate expired or not yet valid */
    WE_SCHEME,              /* unsupported URL scheme */
    WE_NOTFOUND,            /* file: or res: without that file */
    WE_NOMEM,
    WE_REDIRECTS,
    WE_BADURL,
    WE_DECODE,              /* bad gzip or image data */
};
const char *web_strerror(int err);

/* ---- arena --------------------------------------------------------------- */

/* Bump allocator for everything a page owns. Reset keeps its chunks, since
 * Glaze can't return memory to the kernel. */
struct arena_chunk;
struct arena {
    struct arena_chunk *chunks, *cur;
    size_t used;
    bool failed;            /* an allocation failed since the last reset */
};

void *arena_alloc(struct arena *a, size_t n);       /* zeroed, 8-aligned */
char *arena_strndup(struct arena *a, const char *s, int n);
void arena_reset(struct arena *a);
void arena_free(struct arena *a);

/* ---- charset ------------------------------------------------------------- */

enum { CS_UTF8, CS_1252 };

/* Charset label ("utf-8", "iso-8859-1", ...) to CS_*, or -1. */
int cs_from_label(const char *label, int len);
/* From Content-Type's charset, a BOM, or a <meta> in the first 1 KiB. */
int cs_sniff(const char *content_type, const uint8_t *data, int len);
/* Decode to engine text (malloc'd, NUL-terminated). */
char *cs_decode(int cs, const uint8_t *in, int len, int *out_len);
/* Engine text for one code point: writes 0-5 bytes, returns the count. */
int cs_put(uint32_t cp, char *out);

/* ---- DOM ------------------------------------------------------------------ */

#define TAG_LIST(X) \
    X(HTML, "html") X(HEAD, "head") X(BODY, "body") X(TITLE, "title") X(META, "meta") \
    X(LINK, "link") X(STYLE, "style") X(SCRIPT, "script") X(NOSCRIPT, "noscript") X(BASE, "base") \
    X(A, "a") X(B, "b") X(I, "i") X(U, "u") X(S, "s") X(STRONG, "strong") X(EM, "em") \
    X(CODE, "code") X(PRE, "pre") X(TT, "tt") X(KBD, "kbd") X(SAMP, "samp") X(VAR, "var") \
    X(CITE, "cite") X(DFN, "dfn") X(Q, "q") X(SMALL, "small") X(BIG, "big") X(SUB, "sub") \
    X(SUP, "sup") X(SPAN, "span") X(FONT, "font") X(BR, "br") X(HR, "hr") X(P, "p") \
    X(DIV, "div") X(H1, "h1") X(H2, "h2") X(H3, "h3") X(H4, "h4") X(H5, "h5") X(H6, "h6") \
    X(UL, "ul") X(OL, "ol") X(LI, "li") X(DL, "dl") X(DT, "dt") X(DD, "dd") X(MENU, "menu") \
    X(DIR, "dir") X(BLOCKQUOTE, "blockquote") X(CENTER, "center") X(IMG, "img") \
    X(TABLE, "table") X(CAPTION, "caption") X(THEAD, "thead") X(TBODY, "tbody") \
    X(TFOOT, "tfoot") X(TR, "tr") X(TD, "td") X(TH, "th") X(COL, "col") X(COLGROUP, "colgroup") \
    X(FORM, "form") X(INPUT, "input") X(BUTTON, "button") X(SELECT, "select") \
    X(OPTION, "option") X(OPTGROUP, "optgroup") X(TEXTAREA, "textarea") X(LABEL, "label") \
    X(FIELDSET, "fieldset") X(LEGEND, "legend") X(IFRAME, "iframe") X(OBJECT, "object") \
    X(EMBED, "embed") X(PARAM, "param") X(NAV, "nav") X(HEADER, "header") X(FOOTER, "footer") \
    X(SECTION, "section") X(ARTICLE, "article") X(ASIDE, "aside") X(MAIN, "main") \
    X(FIGURE, "figure") X(FIGCAPTION, "figcaption") X(ADDRESS, "address") X(STRIKE, "strike") \
    X(DEL, "del") X(INS, "ins") X(MARK, "mark") X(ABBR, "abbr") X(ACRONYM, "acronym") \
    X(SVG, "svg") X(CANVAS, "canvas") X(VIDEO, "video") X(AUDIO, "audio") X(PICTURE, "picture") \
    X(SOURCE, "source") X(TRACK, "track") X(WBR, "wbr") X(AREA, "area") X(MAP, "map") \
    X(FRAMESET, "frameset") X(FRAME, "frame") X(NOFRAMES, "noframes") X(TEMPLATE, "template") \
    X(DETAILS, "details") X(SUMMARY, "summary") X(XMP, "xmp") X(LISTING, "listing") \
    X(PLAINTEXT, "plaintext") X(NOBR, "nobr") X(HGROUP, "hgroup") X(DATALIST, "datalist") \
    X(TIME, "time") X(DIALOG, "dialog") X(MATH, "math")

enum {
    T_UNKNOWN = 0,
#define X(id, name) T_##id,
    TAG_LIST(X)
#undef X
    T_COUNT
};

int tag_lookup(const char *name, int len);
const char *tag_name(int tag);

struct attr {
    struct attr *next;
    const char *name;           /* lowercase */
    const char *value;
};

enum { N_DOC, N_ELEM, N_TEXT, N_FRAGMENT };

struct style;
struct ctl;
struct page;
struct frame_slot;
struct css_selector;

struct node {
    uint8_t type;
    uint16_t tag;
    struct node *parent, *first, *last, *next, *prev;
    struct attr *attrs;
    const char *name;           /* tag name as written, lowercase */
    char *text;                 /* N_TEXT: raw text */
    int len;
    const char *ctext;          /* N_TEXT: collapsed text (layout cache) */
    int clen;
    uint8_t ckey;               /* white-space and transform ctext was made for */
    struct style *st;           /* computed style (display D_NONE when hidden) */
    struct css_decl *sdecl;     /* parsed style="" attribute */
    int16_t nsdecl;             /* -1 until parsed */
    struct ctl *ctl;            /* form controls */
    int img;                    /* <img>: image slot, or -1 */
    bool image_event;
    struct page *owner;
    int frame;                  /* child browsing context slot, or -1 */
    uint8_t collected;
};

const char *node_attr(const struct node *n, const char *name);
bool node_has_class(const struct node *n, const char *cls, int len);
struct node *node_first_elem(const struct node *n);
/* Next node in document order within root (descends into children unless
 * skip_children). */
struct node *node_next(const struct node *n, const struct node *root, bool skip_children);
/* Concatenated text of the subtree (arena), for <title>, <option>, ... */
char *node_text(struct arena *a, const struct node *n);
struct node *dom_create(struct page *p, int type, const char *name, const char *text);
bool dom_insert(struct page *p, struct node *parent, struct node *child, struct node *before);
void dom_remove(struct page *p, struct node *n);
bool dom_attr(struct page *p, struct node *n, const char *name, const char *value);
bool dom_content(struct page *p, struct node *n, const char *text, bool html);
char *dom_serialize(struct page *p, struct node *n, bool outer);
struct node *dom_clone(struct page *p, struct node *n, bool deep);
struct node *dom_query(struct page *p, struct node *root, const char *selector, struct node *after);
int dom_select(struct page *p,struct node *root,const char *selector,struct node **out,int max);
bool css_node_matches(const struct css_selector *selector,const struct node *n);

/* ---- HTML ----------------------------------------------------------------- */

#define HTML_MAX_DEPTH  64
#define HTML_MAX_NODES  30000

/* Parses engine text into a tree under a N_DOC node, which always holds
 * html > head, body. Scripting mode controls noscript tokenization. */
struct node *html_parse(struct arena *a, const char *src, int len, bool scripting_enabled);
/* Decodes character references in [s, s+n) into out (at most n bytes, as
 * they never grow); returns the length. */
int html_unescape(const char *s, int n, char *out, bool in_attr);

/* ---- CSS ------------------------------------------------------------------ */

enum {
    V_NONE = 0, V_KEYWORD, V_LENGTH, V_PERCENT, V_NUMBER, V_COLOR, V_AUTO, V_INHERIT,
};
enum { U_PX, U_EM, U_REM, U_EX, U_PT, U_PC, U_IN, U_CM, U_MM, U_VW, U_VH, U_CH };

#define KW_LIST(X) \
    X(NONE_, "none") X(BLOCK, "block") X(INLINE, "inline") X(INLINE_BLOCK, "inline-block") \
    X(LIST_ITEM, "list-item") X(TABLE, "table") X(INLINE_TABLE, "inline-table") \
    X(TABLE_ROW, "table-row") X(TABLE_CELL, "table-cell") X(TABLE_ROW_GROUP, "table-row-group") \
    X(TABLE_HEADER_GROUP, "table-header-group") X(TABLE_FOOTER_GROUP, "table-footer-group") \
    X(TABLE_CAPTION, "table-caption") X(TABLE_COLUMN, "table-column") \
    X(TABLE_COLUMN_GROUP, "table-column-group") X(FLEX, "flex") X(INLINE_FLEX, "inline-flex") \
    X(GRID, "grid") X(INLINE_GRID, "inline-grid") X(CONTENTS, "contents") X(FLOW_ROOT, "flow-root") \
    X(NORMAL, "normal") X(BOLD, "bold") X(BOLDER, "bolder") X(LIGHTER, "lighter") \
    X(ITALIC, "italic") X(OBLIQUE, "oblique") X(LEFT, "left") X(RIGHT, "right") \
    X(CENTER, "center") X(JUSTIFY, "justify") X(START, "start") X(END, "end") \
    X(UNDERLINE, "underline") X(LINE_THROUGH, "line-through") X(OVERLINE, "overline") \
    X(NOWRAP, "nowrap") X(PRE, "pre") X(PRE_WRAP, "pre-wrap") X(PRE_LINE, "pre-line") \
    X(BREAK_SPACES, "break-spaces") X(DISC, "disc") X(CIRCLE, "circle") X(SQUARE, "square") \
    X(DECIMAL, "decimal") X(LOWER_ALPHA, "lower-alpha") X(UPPER_ALPHA, "upper-alpha") \
    X(LOWER_LATIN, "lower-latin") X(UPPER_LATIN, "upper-latin") X(LOWER_ROMAN, "lower-roman") \
    X(UPPER_ROMAN, "upper-roman") X(UPPERCASE, "uppercase") X(LOWERCASE, "lowercase") \
    X(CAPITALIZE, "capitalize") X(HIDDEN, "hidden") X(VISIBLE, "visible") X(COLLAPSE, "collapse") \
    X(SOLID, "solid") X(DOTTED, "dotted") X(DASHED, "dashed") X(DOUBLE, "double") \
    X(GROOVE, "groove") X(RIDGE, "ridge") X(INSET, "inset") X(OUTSET, "outset") \
    X(SUB, "sub") X(SUPER, "super") X(TOP, "top") X(MIDDLE, "middle") X(BOTTOM, "bottom") \
    X(BASELINE, "baseline") X(TEXT_TOP, "text-top") X(TEXT_BOTTOM, "text-bottom") \
    X(THIN, "thin") X(MEDIUM, "medium") X(THICK, "thick") X(XX_SMALL, "xx-small") \
    X(X_SMALL, "x-small") X(SMALL, "small") X(LARGE, "large") X(X_LARGE, "x-large") \
    X(XX_LARGE, "xx-large") X(SMALLER, "smaller") X(LARGER, "larger") \
    X(MONOSPACE, "monospace") X(SANS, "sans-serif") X(TRANSPARENT, "transparent") \
    X(CURRENTCOLOR, "currentcolor") X(STATIC, "static") X(RELATIVE, "relative") \
    X(ABSOLUTE, "absolute") X(FIXED, "fixed") X(STICKY, "sticky") X(BOTH, "both") \
    X(INSIDE, "inside") X(OUTSIDE, "outside") X(SCROLL, "scroll") X(CLIP, "clip")

enum {
    K_UNKNOWN = 0,
#define X(id, name) K_##id,
    KW_LIST(X)
#undef X
    K_COUNT
};

int kw_lookup(const char *s, int len);

enum {
    P_DISPLAY, P_COLOR, P_BACKGROUND_COLOR, P_FONT_SIZE, P_FONT_WEIGHT, P_FONT_STYLE,
    P_FONT_FAMILY, P_TEXT_ALIGN, P_TEXT_DECORATION, P_TEXT_INDENT, P_TEXT_TRANSFORM,
    P_WHITE_SPACE, P_LINE_HEIGHT,
    P_MARGIN_TOP, P_MARGIN_RIGHT, P_MARGIN_BOTTOM, P_MARGIN_LEFT,
    P_PADDING_TOP, P_PADDING_RIGHT, P_PADDING_BOTTOM, P_PADDING_LEFT,
    P_BORDER_TOP_WIDTH, P_BORDER_RIGHT_WIDTH, P_BORDER_BOTTOM_WIDTH, P_BORDER_LEFT_WIDTH,
    P_BORDER_TOP_STYLE, P_BORDER_RIGHT_STYLE, P_BORDER_BOTTOM_STYLE, P_BORDER_LEFT_STYLE,
    P_BORDER_TOP_COLOR, P_BORDER_RIGHT_COLOR, P_BORDER_BOTTOM_COLOR, P_BORDER_LEFT_COLOR,
    P_WIDTH, P_MAX_WIDTH, P_MIN_WIDTH, P_HEIGHT, P_MAX_HEIGHT, P_LIST_STYLE_TYPE,
    P_LIST_STYLE_POSITION, P_VERTICAL_ALIGN, P_VISIBILITY, P_FLOAT, P_CLEAR, P_POSITION,
    P_LEFT, P_TOP, P_OVERFLOW,
    P_COUNT
};

struct css_value {
    uint8_t type;               /* V_* */
    uint8_t unit;               /* U_* for V_LENGTH */
    int32_t num;                /* 1/64ths; keyword id; colour 0xAARRGGBB */
};

struct css_decl {
    uint8_t prop;
    bool important;
    struct css_value v;
};

enum { SC_NONE, SC_DESC, SC_CHILD, SC_ADJ, SC_SIB };
enum { PS_LINK = 1, PS_FIRST_CHILD = 2, PS_LAST_CHILD = 4, PS_ROOT = 8, PS_NEVER = 0x80 };
enum { AO_EXISTS, AO_EQ, AO_WORD, AO_PREFIX, AO_SUFFIX, AO_SUBSTR, AO_DASH };

struct css_attrsel {
    const char *name, *value;
    uint8_t op;
};

/* One compound selector; parts[] run left to right and each part's
 * combinator links it to the part before it. */
struct css_compound {
    uint16_t tag;               /* T_*, 0 = any */
    uint8_t comb;               /* SC_* to the previous part */
    uint8_t pseudo;             /* PS_* */
    uint8_t ncls, nattr;
    const char *tagname;        /* for T_UNKNOWN tags */
    const char *id;
    const char **cls;
    struct css_attrsel *attrs;
};

struct css_selector {
    struct css_compound *parts;
    uint16_t nparts;
    uint32_t spec;
};

struct css_rule {
    struct css_selector *sels;
    struct css_decl *decls;
    uint16_t nsels, ndecls;
};

struct css_index_entry {
    uint32_t key;               /* bucket kind in the top bits, hash below */
    uint16_t sel;
    int rule;
};

struct stylesheet {
    struct css_rule *rules;     /* malloc'd */
    int nrules, cap;
    struct css_index_entry *index;
    int nindex;
    bool loaded;
};

void css_parse_sheet(struct arena *a, struct stylesheet *ss, const char *src, int len);
void css_free_sheet(struct stylesheet *ss);
/* Parses a declaration block (style="..."); returns the count. */
int css_parse_decls(struct arena *a, const char *src, int len, struct css_decl **out);
/* Colour: 0xFFRRGGBB, or 0 for transparent. */
bool css_parse_color(const char *s, int len, uint32_t *out);
/* Rule index keys: kind 0 universal, 1 tag, 2 class, 3 id. */
uint32_t css_key(int kind, uint32_t h);
uint32_t css_hash(const char *s);

/* ---- computed style --------------------------------------------------------- */

enum {
    D_NONE, D_INLINE, D_BLOCK, D_INLINE_BLOCK, D_LIST_ITEM, D_TABLE, D_TABLE_ROW_GROUP,
    D_TABLE_ROW, D_TABLE_CELL, D_TABLE_CAPTION, D_TABLE_COLUMN,
};
enum { WS_NORMAL, WS_NOWRAP, WS_PRE, WS_PRE_WRAP, WS_PRE_LINE };
enum { TA_LEFT, TA_RIGHT, TA_CENTER, TA_JUSTIFY };
enum { VA_BASELINE, VA_SUB, VA_SUPER, VA_TOP, VA_MIDDLE, VA_BOTTOM };
enum { LS_NONE, LS_DISC, LS_CIRCLE, LS_SQUARE, LS_DECIMAL, LS_LOWER_ALPHA, LS_UPPER_ALPHA,
       LS_LOWER_ROMAN, LS_UPPER_ROMAN };
enum { TT_NONE, TT_UPPER, TT_LOWER, TT_CAPITALIZE };
enum { DECO_UNDERLINE = 1, DECO_STRIKE = 2, DECO_OVERLINE = 4 };

/* A length kept until layout knows the containing width. */
enum { L_AUTO, L_PX, L_PCT };
struct len {
    int16_t v;                  /* device px, or percent * 64 */
    uint8_t unit;
};

struct style {
    uint8_t display, white_space, text_align, valign, list_style, transform, deco;
    bool bold, italic, mono, hidden, flex;
    int16_t font_css;           /* CSS px * 4, inherited for em */
    int16_t line_h;             /* device px, 0 = normal */
    uint32_t color;             /* 0xRRGGBB */
    uint32_t bg;                /* 0xFFRRGGBB, 0 = transparent */
    struct len margin[4], padding[4];   /* top, right, bottom, left */
    struct len width, max_width, min_width, height, text_indent;
    int16_t border_w[4];
    uint32_t border_c[4];
    const gfx_font *font;
};

/* Fonts available for a size in device px. */
const gfx_font *web_font(int px, bool bold, bool italic, bool mono);

/* ---- form controls ------------------------------------------------------------ */

enum { C_TEXT, C_PASSWORD, C_CHECKBOX, C_RADIO, C_SUBMIT, C_RESET, C_BUTTON, C_HIDDEN,
       C_IMAGE, C_SELECT, C_TEXTAREA, C_FILE };

struct ctl {
    struct ctl *next;
    struct node *node, *form;
    uint8_t type;
    bool checked, disabled;
    int sel;                    /* C_SELECT: selected option */
    int nopts;
    struct node **opts;
    char *value;                /* text value (arena, cap bytes) */
    int len, cap;
    int cursor;                /* insertion position while focused */
    int selection_start, selection_end; /* user selection while focused */
    const char *init;           /* initial value, for reset */
    bool init_checked;
    int init_sel;
    int size, rows;             /* in characters / lines */
};

struct page;
void forms_collect(struct page *p);
const char *ctl_label(const struct ctl *c);     /* button or select text */
/* Builds the submission for form f (submitter may be NULL): returns a
 * malloc'd URL for GET, or the action URL with *post set for POST. */
char *forms_submit(struct page *p, struct node *form, struct ctl *submitter, char **post);
void forms_reset(struct page *p, struct node *form);
void ctl_toggle(struct page *p, struct ctl *c);

/* ---- page, layout and painting ----------------------------------------------- */

enum { DI_RECT, DI_TEXT, DI_IMAGE, DI_CTL, DI_BULLET, DI_FRAME, DI_BOX };

struct ditem {
    uint8_t kind, deco;
    int16_t x, w, h;
    int32_t y;
    uint32_t color;
    struct node *link;          /* enclosing <a href>, or NULL */
    struct node *node;          /* DOM event target */
    union {
        struct {
            const gfx_font *f;
            const char *s;
            int n;
        } t;
        int img;
        struct ctl *ctl;
        uint8_t bullet;         /* LS_DISC ... */
        struct frame_slot *frame;
    } u;
};

enum { IMG_NONE, IMG_WAIT, IMG_LOADING, IMG_OK, IMG_FAILED };

struct image_slot {
    char *url;                  /* absolute, malloc'd */
    uint8_t state;
    int w, h;                   /* decoded size, device px */
    int want_w, want_h;         /* size from the <img> attributes, device px */
    gfx_image img;
    void *mem;                  /* pixels + alpha */
    int fetch;                  /* loader id while loading */
};

#define PAGE_MAX_SHEETS 24
#define PAGE_MAX_SCRIPTS 32
#define PAGE_MAX_FRAMES 8
#define FRAME_MAX_DEPTH 3

enum { RES_WAIT, RES_LOADING, RES_READY, RES_FAILED, RES_DONE };
struct script_slot {
    struct node *node;
    char *url, *source;
    int fetch, len;
    bool module;
    uint8_t state;
};
struct frame_slot {
    struct page *owner, *page;
    struct node *node;
    char *url, *post;
    int fetch, x,y,w,h,scroll;
    uint8_t state;
    bool sandboxed, loaded_event, inline_document;
};

struct sheet_slot {
    struct stylesheet ss;
    struct node *node;
    char *url;                  /* <link>: absolute URL, malloc'd */
    int fetch;                  /* loader id while loading */
    bool pending;
};

struct page {
    struct arena arena;
    char *url;                  /* final URL, malloc'd */
    char *base;                 /* <base href> or url */
    char *title;                /* arena */
    char *src;                  /* decoded document (malloc'd) */
    int src_len;
    bool is_image;              /* the document itself is an image */
    struct node *doc, *html, *body;
    struct sheet_slot sheets[PAGE_MAX_SHEETS];
    int nsheets;
    struct image_slot *imgs;    /* malloc'd */
    int nimgs, img_cap;
    uint32_t img_bytes;
    struct ctl *ctls;
    struct ditem *items;        /* malloc'd */
    int nitems, item_cap;
    struct anchor {
        const char *name;
        int y;
    } *anchors;                 /* ids and <a name>s, malloc'd */
    int nanchors, anchor_cap;
    int content_h, width;
    uint32_t bg;                /* canvas colour */
    uint32_t link_color;        /* <body link=...> or 0 */
    bool oom;                   /* something didn't fit in memory */
    bool is_error;              /* load_error_page's */
    struct page *parent;
    struct frame_slot *host;
    struct script_slot scripts[PAGE_MAX_SCRIPTS];
    struct frame_slot frames[PAGE_MAX_FRAMES];
    int nscripts,nframes,script_next,dom_nodes;
    int viewport_h, scroll_y;
    unsigned serial;
    uint32_t start_ms;
    double time_origin;
    void *js;
    char *origin, *navigate_url, *navigate_post, *csp;
    bool scripting_enabled;    /* document parsing mode; survives runtime failures */
    bool dirty,resources_dirty,ready_event,load_event,script_disabled,opaque_origin;
};

struct page *page_new(const char *url);
void page_free(struct page *p);
/* Builds the DOM from a fetched document and collects its subresources. */
void page_set_document(struct page *p, const uint8_t *data, int len, const char *content_type);
/* Engine text (already decoded), e.g. generated error pages. */
void page_set_html(struct page *p, const char *html);
void page_style(struct page *p);
void page_collect(struct page *p);
void page_layout(struct page *p, int width);
/* Position of the element with this id / anchor name, or -1. */
int page_anchor_y(struct page *p, const char *frag);
/* Display item at content coordinates, preferring links and controls. */
struct ditem *page_hit(struct page *p, int x, int y);
struct frame_slot *page_frame_at(struct page *p, int x, int y);
struct page *page_root(struct page *p);
bool page_same_origin(const struct page *a,const struct page *b);
bool page_resource_allowed(const struct page *p,const char *url);
bool page_navigate(struct page *p,const char *url,const char *target,const char *post);
bool frame_response_allowed(struct page *parent,const char *url,const char *xfo,const char *csp);
/* The <a href> element's resolved URL (malloc'd) or NULL. */
char *page_link_url(struct page *p, struct node *a);
const char *page_refresh(struct page *p, int *delay_s);

/* Image slots: sniffed and decoded into device-sized RGB565 + alpha. */
int page_add_image(struct page *p, const char *url);
bool image_decode(struct page *p, struct image_slot *s, const uint8_t *data, int len);
void image_release(struct image_slot *s);
/* The document itself is an image (an image/ Content-Type). */
bool image_document(struct page *p, const uint8_t *data, int len);

/* Paints the content rows [scroll + clip.y - view.y, ...) of the page into
 * s, where view is the page viewport on s. hilite is a link to highlight. */
void paint_page(struct page *p, gfx_surface *s, gfx_rect view, int scroll, gfx_rect clip,
                const struct node *hilite, const struct ctl *focus);

/* Style and layout internals shared between style.c and layout.c. */
void style_tree(struct page *p, struct stylesheet *ua);
struct stylesheet *style_ua_sheet(struct arena *a);

#endif
