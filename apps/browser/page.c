/* A page: its document, stylesheets, images and layout. */

#include "url.h"
#include "web.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct page *page_new(const char *url)
{
    struct page *p = calloc(1, sizeof(*p));

    if (!p)
        return NULL;
    p->url = strdup(url);
    p->base = strdup(url);
    p->bg = 0xFFFFFF;
    if (!p->url || !p->base) {
        page_free(p);
        return NULL;
    }
    return p;
}

void page_free(struct page *p)
{
    if (!p)
        return;
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

/* After parsing: title, base, stylesheets, images and controls. */
static void collect(struct page *p)
{
    p->html = node_first_elem(p->doc);
    p->body = NULL;
    for (struct node *c = p->html ? p->html->first : NULL; c; c = c->next)
        if (c->type == N_ELEM && c->tag == T_BODY)
            p->body = c;

    for (struct node *n = p->doc; n; n = node_next(n, p->doc, false)) {
        if (n->type != N_ELEM)
            continue;
        switch (n->tag) {
        case T_TITLE:
            if (!p->title)
                p->title = node_text(&p->arena, n);
            break;
        case T_BASE: {
            const char *href = node_attr(n, "href");
            char *b = href ? url_resolve(p->url, href) : NULL;
            if (b) {
                free(p->base);
                p->base = b;
            }
            break;
        }
        case T_STYLE: {
            const char *media = node_attr(n, "media");
            if (p->nsheets >= PAGE_MAX_SHEETS || (media && strstr(media, "print") && !strstr(media, "screen")))
                break;
            struct sheet_slot *sl = &p->sheets[p->nsheets++];
            memset(sl, 0, sizeof(*sl));
            if (n->first && n->first->type == N_TEXT)
                css_parse_sheet(&p->arena, &sl->ss, n->first->text, n->first->len);
            sl->ss.loaded = true;
            break;
        }
        case T_LINK: {
            const char *rel = node_attr(n, "rel"), *href = node_attr(n, "href");
            const char *media = node_attr(n, "media");
            if (!rel_has(rel, "stylesheet") || rel_has(rel, "alternate") || !href ||
                p->nsheets >= PAGE_MAX_SHEETS || (media && strstr(media, "print") && !strstr(media, "screen")))
                break;
            struct sheet_slot *sl = &p->sheets[p->nsheets];
            memset(sl, 0, sizeof(*sl));
            sl->url = url_resolve(p->base, href);
            if (sl->url) {
                sl->pending = true;
                p->nsheets++;
            }
            break;
        }
        case T_BODY: {
            uint32_t c;
            const char *v = node_attr(n, "link");
            if (v && css_parse_color(v, (int)strlen(v), &c) && c)
                p->link_color = c & 0xFFFFFF;
            break;
        }
        case T_IMG: {
            /* Lazy loaders keep the real address in data-src. */
            const char *src = node_attr(n, "data-src");
            if (!src || !*src)
                src = node_attr(n, "src");
            if (!src || !*src)
                break;
            char *url = url_resolve(p->base, src);
            if (url) {
                n->img = page_add_image(p, url);
                if (n->img >= 0 && !p->imgs[n->img].want_w) {
                    p->imgs[n->img].want_w = attr_px(node_attr(n, "width"));
                    p->imgs[n->img].want_h = attr_px(node_attr(n, "height"));
                }
                free(url);
            }
            break;
        }
        }
    }
    forms_collect(p);
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
    p->doc = html_parse(&p->arena, p->src, p->src_len);
    if (!p->doc) {
        p->oom = true;
        return;
    }
    collect(p);
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
    p->doc = html_parse(&p->arena, p->src, p->src_len);
    if (!p->doc) {
        p->oom = true;
        return;
    }
    collect(p);
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
