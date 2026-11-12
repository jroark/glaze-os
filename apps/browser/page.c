/* A page: its document, stylesheets, images and layout. */

#include "url.h"
#include "web.h"
#include "frames.h"
#include "script.h"
#include "net.h"
#include "plat.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static unsigned page_serial;

struct page *page_new(const char *url)
{
    struct page *p = calloc(1, sizeof(*p));

    if (!p)
        return NULL;
    p->url = strdup(url);
    p->base = strdup(url);
    p->bg = 0xFFFFFF;
    p->scripting_enabled = true;
    p->start_ms = plat_ms();
    p->time_origin = (double)plat_time() * 1000.0;
    p->origin=strdup(url);p->serial=++page_serial;p->viewport_h=226;p->want_scroll=-1;p->dirty=p->resources_dirty=true;
    if (!p->url || !p->base || !p->origin) {
        page_free(p);
        return NULL;
    }
    return p;
}

void page_free(struct page *p)
{
    if (!p)
        return;
    script_free(p);
    for(int i=0;i<p->nscripts;i++) { free(p->scripts[i].url);free(p->scripts[i].source); }
    for(int i=0;i<p->nframes;i++) { frame_cancel(&p->frames[i]);free(p->frames[i].url);free(p->frames[i].post); }
    free(p->origin);free(p->navigate_url);free(p->navigate_post);free(p->csp);
    for (int i = 0; i < p->nimgs; i++) {
        image_release(&p->imgs[i]);
        free(p->imgs[i].url);
    }
    css_vars_free(&p->vars);
    for (int i = 0; i < p->nsheets; i++) {
        css_free_sheet(&p->sheets[i].ss);
        free(p->sheets[i].url);
    }
    free(p->imgs);
    free(p->items);
    free(p->anchors);
    free(p->src);
    free(p->url);
    free(p->base);
    arena_free(&p->arena);
    free(p);
}

int page_add_image(struct page *p, const char *url)
{
    for (int i = 0; i < p->nimgs; i++)
        if (!strcmp(p->imgs[i].url, url))
            return i;
    if(p->nimgs>=128)return -1;
    if (p->nimgs == p->img_cap) {
        int cap = p->img_cap ? p->img_cap * 2 : 16;
        struct image_slot *n = realloc(p->imgs, sizeof(*n) * (size_t)cap);
        if (!n)
            return -1;
        p->imgs = n;
        p->img_cap = cap;
    }
    struct image_slot *s = &p->imgs[p->nimgs];
    memset(s, 0, sizeof(*s));
    s->url = strdup(url);
    if (!s->url)
        return -1;
    s->state = IMG_WAIT;
    return p->nimgs++;
}

/* Device px from an <img> size attribute ("120", "120px"; not "50%"). */
static int attr_px(const char *v)
{
    if (!v || !isdigit((unsigned char)*v))
        return 0;
    char *end;
    long n = strtol(v, &end, 10);
    if (*end == '%' || n <= 0 || n > 4000)
        return 0;
    return CSS_TO_DEV((int)n) ? CSS_TO_DEV((int)n) : 1;
}

static bool rel_has(const char *rel, const char *word)
{
    size_t wl = strlen(word);

    for (const char *r = rel; r && *r;) {
        while (*r == ' ')
            r++;
        const char *s = r;
        while (*r && *r != ' ')
            r++;
        if ((size_t)(r - s) == wl && !strncasecmp(s, word, wl))
            return true;
    }
    return false;
}

/* Idempotent resource discovery after parsing or a DOM mutation. */
/* Whether a <script type> is JavaScript: none, "module", or one of the
 * names it has gone by, with any ";charset=..." parameter ignored. */
static bool script_type(const char *type)
{
    static const char *const names[] = {
        "module", "text/javascript", "application/javascript", "text/ecmascript", "application/ecmascript",
        "application/x-javascript", "text/x-javascript", "text/jscript", "text/livescript",
        "text/javascript1.0", "text/javascript1.1", "text/javascript1.2", "text/javascript1.3",
        "text/javascript1.4", "text/javascript1.5",
    };
    int len = 0;

    if (!type)
        return true;
    while (*type == ' ')
        type++;
    while (type[len] && type[len] != ';' && type[len] != ' ')
        len++;
    if (!len)
        return true;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if ((int)strlen(names[i]) == len && !strncasecmp(type, names[i], (size_t)len))
            return true;
    return false;
}

/* ---- which image to fetch --------------------------------------------------- */

/* Whether a URL's path ends in the extension of a format with no decoder. */
static bool undecodable(const char *url, int len)
{
    static const char *const ext[] = { ".webp", ".avif", ".svg", ".jxl" };
    int end = 0;

    while (end < len && url[end] != '?' && url[end] != '#')
        end++;
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) {
        int el = (int)strlen(ext[i]);
        if (end >= el && !strncasecmp(url + end - el, ext[i], (size_t)el))
            return true;
    }
    return false;
}

/*
 * The best candidate of a srcset for a 240-pixel screen: the narrowest one at
 * least that wide (or the widest there is), or the lowest density. Formats
 * the decoder doesn't have lose to any it does. Copies it to out; false when
 * the list has nothing usable.
 */
static bool srcset_pick(const char *set, char *out, int cap)
{
    const char *best = NULL;
    int best_len = 0, best_score = 0;
    bool best_bad = true;

    for (const char *s = set; *s;) {
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == ',')
            s++;
        const char *url = s;
        while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r')
            s++;
        int len = (int)(s - url);
        while (len && url[len - 1] == ',')
            len--;                      /* "a.png, b.png 2x" */
        if (!len)
            break;
        /* The descriptor: "480w" or "2x"; none means 1x. */
        long value = 0;
        char kind = 'x';
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
            s++;
        if (*s && *s != ',' && s[-1] != ',' && url[len] != ',') {
            /* A number with up to two decimals, in hundredths. */
            const char *e = s;
            long whole = 0, frac = 0, digits = 0;
            for (; *e >= '0' && *e <= '9'; e++)
                whole = whole < 100000 ? whole * 10 + (*e - '0') : whole;
            if (*e == '.')
                for (e++; *e >= '0' && *e <= '9'; e++)
                    if (digits < 2) {
                        frac = frac * 10 + (*e - '0');
                        digits++;
                    }
            if (digits == 1)
                frac *= 10;
            if (e != s && (*e == 'w' || *e == 'x')) {
                kind = *e;
                value = kind == 'w' ? whole : whole * 100 + frac;
            }
            while (*s && *s != ',')
                s++;
        }
        if (kind == 'x' && !value)
            value = 100;
        /* Lower is better: widths below the screen rank after all that fit. */
        int score = kind == 'w' ? (value >= CSS_TO_DEV(WEB_VIEW_CSS_W) ? (int)value : 1000000 - (int)value) : (int)value;
        bool bad = undecodable(url, len);
        if (!best || (best_bad && !bad) || (bad == best_bad && score < best_score)) {
            best = url;
            best_len = len;
            best_score = score;
            best_bad = bad;
        }
    }
    if (!best || best_len >= cap)
        return false;
    memcpy(out, best, (size_t)best_len);
    out[best_len] = 0;
    return true;
}

/*
 * An <img>'s source. Pages serve one image at several sizes (srcset, or
 * <picture> with <source>s), and lazy loaders keep the real address in
 * data-src behind an empty or placeholder src. Taking src alone fetched
 * desktop-sized files, which often broke the size limits, or a 1x1 GIF.
 */
static const char *image_source(struct node *n, char *buf, int cap)
{
    static const char *const lazy[] = { "data-src", "data-original", "data-lazy-src", "data-lazy" };
    const char *src = node_attr(n, "src");

    if (n->parent && n->parent->tag == T_PICTURE)
        for (struct node *s = n->parent->first; s && s != n; s = s->next) {
            const char *set = s->tag == T_SOURCE ? node_attr(s, "srcset") : NULL;
            const char *type = set ? node_attr(s, "type") : NULL, *media = set ? node_attr(s, "media") : NULL;
            if (!set || (media && *media && !css_media_matches(media, (int)strlen(media))))
                continue;
            if (type && strncasecmp(type, "image/jpeg", 10) && strncasecmp(type, "image/png", 9) &&
                strncasecmp(type, "image/gif", 9))
                continue;
            if (srcset_pick(set, buf, cap) && !undecodable(buf, (int)strlen(buf)))
                return buf;
        }
    const char *set = node_attr(n, "srcset");
    if (!set || !*set)
        set = node_attr(n, "data-srcset");
    if (set && *set && srcset_pick(set, buf, cap) && !undecodable(buf, (int)strlen(buf)))
        return buf;
    /* A missing src, or a small inline placeholder, with the real one aside. */
    if (!src || !*src || (!strncasecmp(src, "data:", 5) && strlen(src) < 512))
        for (size_t i = 0; i < sizeof(lazy) / sizeof(lazy[0]); i++) {
            const char *v = node_attr(n, lazy[i]);
            if (v && *v)
                return v;
        }
    return src;
}

/*
 * The sheets the one just parsed into slot `from` imports get slots of their
 * own, placed below it in the cascade (an @import comes before its sheet's
 * own rules). They were skipped, which left a page whose <style> is one
 * @import line with no style at all.
 */
void page_sheet_imports(struct page *p, int from)
{
    const char *urls[8];
    int n = css_imports(urls, 8);
    const struct sheet_slot *by = &p->sheets[from];
    bool nested = (by->order & 15) != 15;

    for (int k = 0; k < n && p->nsheets < PAGE_MAX_SHEETS; k++) {
        char *url = url_resolve(by->url ? by->url : p->base, urls[k]);
        bool seen = !url;
        for (int i = 0; !seen && i < p->nsheets; i++)
            seen = p->sheets[i].url && !strcmp(p->sheets[i].url, url);     /* and import loops */
        if (seen) {
            free(url);
            continue;
        }
        struct sheet_slot *sl = &p->sheets[p->nsheets++];
        memset(sl, 0, sizeof *sl);
        sl->node = by->node;
        sl->url = url;
        sl->pending = true;
        sl->order = (uint16_t)(nested ? by->order : (by->order & ~15) + (k < 14 ? k : 14));
    }
}

void page_collect(struct page *p)
{
    p->html = node_first_elem(p->doc);
    p->body = NULL;
    p->title = NULL;
    for (struct node *c = p->html ? p->html->first : NULL; c; c = c->next)
        if (c->tag == T_BODY)
            p->body = c;
    /* A <template>'s contents are inert: its scripts don't run, and its
     * styles and images aren't fetched. */
    bool inert = false;
    for (struct node *n = p->doc; n; n = node_next(n, p->doc, inert)) {
        inert = n->tag == T_TEMPLATE;
        if (!n->owner) {
            n->owner = p;
            p->dom_nodes++;
        }
        if (n->type != N_ELEM)
            continue;
        if (n->tag == T_TITLE && !p->title)
            p->title = node_text(&p->arena, n);
        if (n->tag == T_BASE && !(n->collected & 1)) {
            char *b = url_resolve(p->url, node_attr(n, "href") ? node_attr(n, "href") : "");
            if (b) {
                free(p->base);
                p->base = b;
            }
            n->collected |= 1;
        }
        if ((n->tag == T_STYLE || n->tag == T_LINK) && !(n->collected & 1)) {
            n->collected |= 1;
            /* Sheets for other media or other widths don't apply: only
             * "print" was checked, so desktop-only sheets were used. */
            const char *media = node_attr(n, "media");
            if (media && *media && !css_media_matches(media, (int)strlen(media)))
                continue;
            if (n->tag == T_LINK &&
                (!rel_has(node_attr(n, "rel"), "stylesheet") ||
                 rel_has(node_attr(n, "rel"), "alternate") || !node_attr(n, "href")))
                continue;
            int i = 0;
            for (; i < p->nsheets; i++)
                if (p->sheets[i].node == n)
                    break;
            if (i == PAGE_MAX_SHEETS)
                continue;
            if (i == p->nsheets)
                p->nsheets++;
            struct sheet_slot *sl = &p->sheets[i];
            if (sl->fetch > 0)
                ld_cancel(sl->fetch);
            css_free_sheet(&sl->ss);
            free(sl->url);
            memset(sl, 0, sizeof *sl);
            sl->node = n;
            sl->order = (uint16_t)(i * 16 + 15);
            if (n->tag == T_STYLE) {
                css_vars_use(&p->vars);
                if (n->first && n->first->type == N_TEXT) {
                    css_parse_sheet(&p->arena, &sl->ss, n->first->text, n->first->len);
                    page_sheet_imports(p, i);
                }
                css_vars_use(NULL);
                sl->ss.loaded = true;
            } else {
                sl->url = url_resolve(p->base, node_attr(n, "href"));
                sl->pending = sl->url != NULL;
            }
        }
        if (n->tag == T_BODY) {
            uint32_t c;
            const char *v = node_attr(n, "link");
            if (v && css_parse_color(v, (int)strlen(v), &c) && c)
                p->link_color = c & 0xffffff;
        }
        if (n->tag == T_IMG && !(n->collected & 2)) {
            n->collected |= 2;
            char chosen[URL_PART_MAX];
            const char *src = image_source(n, chosen, sizeof(chosen));
            if (!src || !*src)
                continue;
            char *url = url_resolve(p->base, src);
            if (url) {
                n->img = page_add_image(p, url);
                if (n->img >= 0) {
                    p->imgs[n->img].want_w = attr_px(node_attr(n, "width"));
                    p->imgs[n->img].want_h = attr_px(node_attr(n, "height"));
                }
                free(url);
            }
        }
        if (n->tag == T_SCRIPT && !(n->collected & 4)) {
            n->collected |= 4;
            bool exists = false;
            for (int i = 0; i < p->nscripts; i++)
                if (p->scripts[i].node == n)
                    exists = true;
            const char *type = node_attr(n, "type");
            bool module = type && !strcasecmp(type, "module");
            /* nomodule marks the fallback bundle for browsers without
             * modules; running it as well ran every such page's code twice. */
            if (exists || p->nscripts == PAGE_MAX_SCRIPTS || !script_type(type) ||
                (!module && node_attr(n, "nomodule")))
                continue;
            struct script_slot *sl = &p->scripts[p->nscripts++];
            memset(sl, 0, sizeof(*sl));
            sl->node = n;
            sl->module = module;
            const char *src = node_attr(n, "src");
            /* An external script marked async, or one a script created and
             * inserted (analytics, ad and widget loaders), runs whenever it
             * arrives and holds nothing up. defer and modules keep their
             * order but come after the plain scripts. */
            sl->async = src && (node_attr(n, "async") || (n->collected & 16));
            sl->defer = !sl->async && (module || (src && node_attr(n, "defer")));
            sl->url = src ? url_resolve(p->base, src) : NULL;
            sl->state = src ? RES_WAIT : RES_READY;
            if ((src && !sl->url) || !script_allowed(p, sl->url, n))
                sl->state = RES_DONE;
        }
        if ((n->tag == T_IFRAME || n->tag == T_FRAME) && !(n->collected & 8)) {
            n->collected |= 8;
            if (n->frame < 0) {
                int depth = 0;
                for (struct page *a = p; a->parent; a = a->parent)
                    depth++;
                if (p->nframes == PAGE_MAX_FRAMES ||
                    frame_count_tree(page_root(p)) >= PAGE_MAX_FRAMES || depth >= FRAME_MAX_DEPTH)
                    continue;
                n->frame = p->nframes++;
                p->frames[n->frame].owner = p;
                p->frames[n->frame].node = n;
            }
            struct frame_slot *f = &p->frames[n->frame];
            const char *src = node_attr(n, "src");
            char *url = url_resolve(p->base, src && *src ? src : "about:blank");
            if (!f->url || !url || strcmp(f->url, url) || node_attr(n, "srcdoc") ||
                f->sandboxed != (node_attr(n, "sandbox") != NULL)) {
                frame_cancel(f);
                free(f->url);
                free(f->post);
                f->post = NULL;
                f->url = url;
                url = NULL;
                f->state = RES_WAIT;
                f->inline_document = node_attr(n, "srcdoc") != NULL;
            }
            f->sandboxed = node_attr(n, "sandbox") != NULL;
            free(url);
        }
    }
    for (int i = 0; i < p->nsheets; i++)
        if (p->sheets[i].node && !node_connected(p->sheets[i].node))
            p->sheets[i].ss.loaded = false;
    forms_collect(p);
    p->resources_dirty = false;
    p->dirty = true;
    if (p->arena.failed)
        p->oom = true;
}

void page_set_html(struct page *p, const char *html)
{
    free(p->src);
    p->src = strdup(html);
    if (!p->src) {
        p->oom = true;
        return;
    }
    p->src_len = (int)strlen(p->src);
    p->doc = html_parse(&p->arena, p->src, p->src_len, p->scripting_enabled);
    if (!p->doc) {
        p->oom = true;
        return;
    }
    page_collect(p);
}

/* Plain text shown as preformatted, escaped. */
static char *text_to_html(const char *s, int n)
{
    static const char head[] = "<pre style=\"white-space:pre-wrap\">";
    size_t cap = sizeof(head) + (size_t)n * 5 + 16;
    char *out = malloc(cap);

    if (!out)
        return NULL;
    size_t o = sizeof(head) - 1;
    memcpy(out, head, o);
    for (int i = 0; i < n; i++) {
        const char *e = s[i] == '<' ? "&lt;" : s[i] == '&' ? "&amp;" : NULL;
        if (e) {
            memcpy(out + o, e, strlen(e));
            o += strlen(e);
        } else {
            out[o++] = s[i];
        }
    }
    out[o] = 0;
    return out;
}

/* Says, at the end of a document that was cut short, that there was more.
 * Built by hand: the scripts' node and arena budgets are spent on such a page. */
static void cut_note(struct page *p)
{
    static const char style[] = "clear:both;margin:12px 0;padding:6px;border-top:1px solid #999;"
                                "color:#666;background:#fff;font:italic 13px sans-serif;text-align:center";
    static const char words[] = "This page is too large to show in full. It ends here.";
    struct node *box = arena_alloc(&p->arena, sizeof *box);
    struct node *text = arena_alloc(&p->arena, sizeof *text);
    struct attr *a = arena_alloc(&p->arena, sizeof *a);

    if (!box || !text || !a || !p->body)
        return;
    text->text = arena_strndup(&p->arena, words, (int)sizeof(words) - 1);
    if (!text->text)
        return;
    box->owner = text->owner = p;
    box->img = box->frame = box->nsdecl = text->img = text->frame = text->nsdecl = -1;
    box->type = N_ELEM;
    box->name = "p";
    box->tag = tag_lookup("p", 1);
    a->name = "style";
    a->value = style;
    box->attrs = a;
    text->type = N_TEXT;
    text->len = (int)sizeof(words) - 1;
    text->parent = box;
    box->first = box->last = text;
    box->parent = p->body;
    box->prev = p->body->last;
    if (box->prev)
        box->prev->next = box;
    else
        p->body->first = box;
    p->body->last = box;
    p->dom_nodes += 2;
}

/*
 * What a response is, from its type and, when the type says little, its first
 * bytes: a page, text to show as written (JSON, XML, a script or sheet opened
 * by itself), a picture, or something the browser has no way to show. Parsing
 * the last two kinds as HTML filled the screen with their bytes.
 */
enum { DOC_HTML, DOC_TEXT, DOC_IMAGE, DOC_OTHER };

static bool ct_is(const char *ct, const char *type)
{
    size_t n = strlen(type);
    return !strncasecmp(ct, type, n) && (!ct[n] || ct[n] == ';' || ct[n] == ' ');
}

static int document_kind(const char *ct, const uint8_t *data, int len)
{
    char sub[64];
    size_t n = strcspn(ct, "; \t");

    if (!strncasecmp(ct, "image/", 6))
        return DOC_IMAGE;
    if (ct_is(ct, "text/html") || ct_is(ct, "application/xhtml+xml"))
        return DOC_HTML;
    snprintf(sub, sizeof(sub), "%.*s", (int)(n < sizeof(sub) - 1 ? n : sizeof(sub) - 1), ct);
    size_t sl = strlen(sub);
    if (!strncasecmp(sub, "text/", 5) || !strcasecmp(sub, "application/json") ||
        !strcasecmp(sub, "application/javascript") || !strcasecmp(sub, "application/x-javascript") ||
        !strcasecmp(sub, "application/ecmascript") || !strcasecmp(sub, "application/xml") ||
        (sl > 5 && (!strcasecmp(sub + sl - 5, "+json") || !strcasecmp(sub + sl - 4, "+xml"))))
        return DOC_TEXT;
    /* No type, or one that says nothing (some servers send every file as
     * application/octet-stream): markup is a page, bytes are not. */
    int i = 0, look = len < 512 ? len : 512;
    while (i < look && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n' ||
                        data[i] >= 0x80))       /* a byte-order mark too */
        i++;
    if (i < look && data[i] == '<')
        return DOC_HTML;
    for (i = 0; i < look; i++)
        if (data[i] < 9 || (data[i] > 13 && data[i] < 32 && data[i] != 27))
            return DOC_OTHER;
    return *ct ? DOC_TEXT : DOC_HTML;
}

void page_set_document(struct page *p, const uint8_t *data, int len, const char *content_type)
{
    const char *ct = content_type ? content_type : "";
    int kind = document_kind(ct, data, len);

    if (kind == DOC_OTHER) {
        p->doc_err = WE_UNSUPPORTED;
        return;
    }
    if (kind == DOC_IMAGE) {
        /* SVG, WebP and AVIF have no decoder here. */
        if (!image_document(p, data, len) && !p->oom)
            p->doc_err = ct_is(ct, "image/png") || ct_is(ct, "image/jpeg") || ct_is(ct, "image/gif")
                         ? WE_DECODE : WE_UNSUPPORTED;
        return;
    }
    int cs = cs_sniff(content_type, data, len), n;
    char *text = cs_decode(cs, data, len, &n);
    if (!text) {
        p->oom = true;
        return;
    }
    if (kind == DOC_TEXT) {
        char *html = text_to_html(text, n);
        free(text);
        if (!html) {
            p->oom = true;
            return;
        }
        page_set_html(p, html);
        free(html);
        if (p->cut && p->doc)
            cut_note(p);
        return;
    }
    free(p->src);
    p->src = text;
    p->src_len = n;
    p->doc = html_parse(&p->arena, p->src, p->src_len, p->scripting_enabled);
    if (!p->doc) {
        p->oom = true;
        return;
    }
    page_collect(p);
    /* The parser stops at its node limit too, with no word of it before. */
    if (p->dom_nodes >= HTML_MAX_NODES)
        p->cut = true;
    if (p->cut)
        cut_note(p);
}

bool image_document(struct page *p, const uint8_t *data, int len)
{
    page_set_html(p, "<body style=\"margin:0;text-align:center\"><img src=\"#\">");
    if (p->oom || !p->nimgs)
        return false;
    p->is_image = true;
    free(p->imgs[0].url);
    p->imgs[0].url = strdup(p->url);
    return image_decode(p, &p->imgs[0], data, len);
}

void page_style(struct page *p)
{
    if (p->doc)
        style_tree(p, style_ua_sheet(NULL));
}

char *page_link_url(struct page *p, struct node *a)
{
    const char *href = node_attr(a, "href");

    return href ? url_resolve(p->base, href) : NULL;
}

const char *page_refresh(struct page *p, int *delay_s)
{
    for (struct node *n = p->doc; n; n = node_next(n, p->doc, false)) {
        if (n->type != N_ELEM || n->tag != T_META)
            continue;
        const char *he = node_attr(n, "http-equiv"), *c = node_attr(n, "content");
        if (!he || strcasecmp(he, "refresh") || !c)
            continue;
        *delay_s = atoi(c);
        const char *u = strchr(c, ';');
        if (!u)
            u = strchr(c, ',');
        if (!u)
            return NULL;
        u++;
        while (*u == ' ')
            u++;
        if (!strncasecmp(u, "url", 3)) {
            u += 3;
            while (*u == ' ')
                u++;
            if (*u == '=')
                u++;
            while (*u == ' ')
                u++;
        }
        if (*u == '\'' || *u == '"')
            u++;
        return u;
    }
    return NULL;
}
