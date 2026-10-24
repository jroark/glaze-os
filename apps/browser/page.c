/* A page: its document, stylesheets, images and layout. */

#include "url.h"
#include "web.h"
#include "frames.h"
#include "script.h"
#include "net.h"

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
    p->origin=strdup(url);p->serial=++page_serial;p->viewport_h=226;p->dirty=p->resources_dirty=true;
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
void page_collect(struct page *p)
{
    p->html = node_first_elem(p->doc);
    p->body = NULL;
    p->title = NULL;
    for (struct node *c = p->html ? p->html->first : NULL; c; c = c->next)
        if (c->tag == T_BODY)
            p->body = c;
    for (struct node *n = p->doc; n; n = node_next(n, p->doc, false)) {
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
            const char *media = node_attr(n, "media");
            if (media && strstr(media, "print") && !strstr(media, "screen"))
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
            if (n->tag == T_STYLE) {
                if (n->first && n->first->type == N_TEXT)
                    css_parse_sheet(&p->arena, &sl->ss, n->first->text, n->first->len);
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
            const char *src = node_attr(n, "src");
            if (!src || !*src)
                src = node_attr(n, "data-src");
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
            if (exists || p->nscripts == PAGE_MAX_SCRIPTS ||
                (type && *type && strcasecmp(type, "text/javascript") &&
                 strcasecmp(type, "application/javascript") && strcasecmp(type, "text/ecmascript")))
                continue;
            struct script_slot *sl = &p->scripts[p->nscripts++];
            sl->node = n;
            const char *src = node_attr(n, "src");
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

void page_set_document(struct page *p, const uint8_t *data, int len, const char *content_type)
{
    const char *ct = content_type ? content_type : "";

    if (!strncasecmp(ct, "image/", 6)) {
        image_document(p, data, len);
        return;
    }
    int cs = cs_sniff(content_type, data, len), n;
    char *text = cs_decode(cs, data, len, &n);
    if (!text) {
        p->oom = true;
        return;
    }
    if (!strncasecmp(ct, "text/plain", 10)) {
        char *html = text_to_html(text, n);
        free(text);
        if (!html) {
            p->oom = true;
            return;
        }
        page_set_html(p, html);
        free(html);
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
