/* Browsing-context geometry, origin checks and target navigation. */
#include "frames.h"
#include "net.h"
#include "url.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct page *page_root(struct page *p)
{
    while (p && p->parent)
        p = p->parent;
    return p;
}
bool node_connected(const struct node *n)
{
    if (!n || !n->owner)
        return false;
    const struct node *root = n->owner->doc;
    for (int d = 0; n && d <= HTML_MAX_DEPTH; d++, n = n->parent)
        if (n == root)
            return true;
    return false;
}
static bool origin_equal(const char *a, const char *b)
{
    struct url x, y;
    if (!a || !b || !url_parse(a, &x) || !url_parse(b, &y) || strcmp(x.scheme, y.scheme))
        return false;
    if (!strcmp(x.scheme, "http") || !strcmp(x.scheme, "https")) {
        int xp = x.port < 0 ? url_default_port(x.scheme) : x.port;
        int yp = y.port < 0 ? url_default_port(y.scheme) : y.port;
        return xp == yp && !strcmp(x.host, y.host);
    }
    if (!strcmp(x.scheme, "res"))
        return true;
    /* File documents and data URLs have distinct origins. srcdoc/about:blank
     * explicitly inherit their creator's origin instead. */
    return !strcmp(x.scheme, "file") && url_same_document(a, b);
}
bool page_same_origin(const struct page *a, const struct page *b)
{
    return a && b &&
           (a == b ||
            (!a->opaque_origin && !b->opaque_origin && origin_equal(a->origin, b->origin)));
}
bool page_resource_allowed(const struct page *p, const char *url)
{
    if (!p || !url || strlen(url) > 32768)
        return false;
    struct url from, to;
    if (!url_parse(p->origin ? p->origin : p->url, &from) || !url_parse(url, &to))
        return false;
    if (!strcmp(to.scheme, "javascript"))
        return false;
    if (!strcmp(to.scheme, "file") || !strcmp(to.scheme, "res"))
        return !strcmp(from.scheme, "file") || !strcmp(from.scheme, "res");
    return !strcmp(to.scheme, "http") || !strcmp(to.scheme, "https") ||
           !strcmp(to.scheme, "data") || !strcmp(url, "about:blank");
}
int frame_count_tree(struct page *p)
{
    int count = p ? p->nframes : 0;
    for (int i = 0; p && i < p->nframes; i++)
        count += frame_count_tree(p->frames[i].page);
    return count;
}
static struct page *named(struct page *p, const char *name)
{
    for (int i = 0; p && i < p->nframes; i++) {
        struct frame_slot *f = &p->frames[i];
        const char *v = node_attr(f->node, "name");
        if (v && !strcmp(v, name) && node_connected(f->node))
            return f->page;
        struct page *child = named(f->page, name);
        if (child)
            return child;
    }
    return NULL;
}
bool page_navigate(struct page *p, const char *url, const char *target, const char *post)
{
    if (!p || !url)
        return false;
    if (!target || !*target) {
        struct node *base = dom_query(p, p->doc, "base[target]", NULL);
        if (base)
            target = node_attr(base, "target");
    }
    struct page *to = p;
    if (target && *target && strcasecmp(target, "_self")) {
        if (!strcasecmp(target, "_parent"))
            to = p->parent ? p->parent : p;
        else if (!strcasecmp(target, "_top") || !strcasecmp(target, "_blank"))
            to = page_root(p);
        else
            to = named(page_root(p), target);
        if (!to)
            return false;
    }
    if (p->host && p->host->sandboxed && (to != p || post))
        return false;
    char *absolute = url_resolve(p->base, url);
    if (!absolute || !page_resource_allowed(p, absolute)) {
        free(absolute);
        return false;
    }
    char *body = post ? strdup(post) : NULL;
    if (post && !body) {
        free(absolute);
        return false;
    }
    free(to->navigate_url);
    free(to->navigate_post);
    to->navigate_url = absolute;
    to->navigate_post = body;
    return true;
}
void frame_cancel(struct frame_slot *f)
{
    if (!f)
        return;
    if (f->fetch > 0)
        ld_cancel(f->fetch);
    f->fetch = 0;
    if (f->page) {
        struct page *p = f->page;
        for (int i = 0; i < p->nsheets; i++)
            if (p->sheets[i].fetch > 0)
                ld_cancel(p->sheets[i].fetch);
        for (int i = 0; i < p->nimgs; i++)
            if (p->imgs[i].fetch > 0)
                ld_cancel(p->imgs[i].fetch);
        for (int i = 0; i < p->nscripts; i++)
            if (p->scripts[i].fetch > 0)
                ld_cancel(p->scripts[i].fetch);
        for (int i = 0; i < p->nframes; i++)
            frame_cancel(&p->frames[i]);
        page_free(p);
        f->page = NULL;
    }
    f->scroll = 0;
    f->loaded_event = false;
}
void frame_geometry(struct frame_slot *f, int x, int y, int w, int h)
{
    if (!f)
        return;
    f->x = x;
    f->y = y;
    f->w = w;
    f->h = h;
    if (f->page && (f->page->width != w || f->page->viewport_h != h)) {
        f->page->viewport_h = h;
        page_layout(f->page, w);
    }
    if (f->page && f->scroll > f->page->content_h - h)
        f->scroll = f->page->content_h - h;
    if (f->scroll < 0)
        f->scroll = 0;
}
/* CSP frame-ancestors is matched against every embedding ancestor. Unsupported
 * source forms fail closed. Multiple policies are checked separately by loader. */
static bool source_matches(const char *source, size_t len, const char *document,
                           const char *ancestor)
{
    if (len == 1 && *source == '*')
        return true;
    if (len == 6 && !strncmp(source, "'self'", 6))
        return origin_equal(document, ancestor);
    char token[2048];
    if (!len || len >= sizeof token)
        return false;
    memcpy(token, source, len);
    token[len] = 0;
    if (token[len - 1] == ':' && !strchr(token, '/')) {
        struct url u;
        return url_parse(ancestor, &u) && strlen(u.scheme) + 1 == len &&
               !strncmp(token, u.scheme, len - 1);
    }
    return origin_equal(token, ancestor);
}
bool frame_response_allowed(struct page *parent, const char *url, const char *xfo, const char *csp)
{
    if (!parent)
        return true;
    if (xfo && *xfo) {
        while (isspace((unsigned char)*xfo))
            xfo++;
        if (strncasecmp(xfo, "SAMEORIGIN", 10))
            return false;
        const char *tail = xfo + 10;
        while (isspace((unsigned char)*tail))
            tail++;
        if (*tail)
            return false;
        for (struct page *p = parent; p; p = p->parent)
            if (!origin_equal(url, p->origin))
                return false;
    }
    const char *s = csp;
    while (s && *s) {
        while (*s && (*s == ';' || isspace((unsigned char)*s)))
            s++;
        const char *end = strchr(s, ';');
        if (!end)
            end = s + strlen(s);
        if (end - s >= 15 && !strncasecmp(s, "frame-ancestors", 15) &&
            (s + 15 == end || isspace((unsigned char)s[15]))) {
            for (struct page *p = parent; p; p = p->parent) {
                bool ok = false;
                const char *t = s + 15;
                while (t < end) {
                    while (t < end && isspace((unsigned char)*t))
                        t++;
                    const char *e = t;
                    while (e < end && !isspace((unsigned char)*e))
                        e++;
                    if (source_matches(t, (size_t)(e - t), url, p->origin))
                        ok = true;
                    t = e;
                }
                if (!ok)
                    return false;
            }
        }
        s = *end ? end + 1 : NULL;
    }
    return true;
}
static int tracks(const char *spec, int total, int fallback, int out[PAGE_MAX_FRAMES])
{
    int count = 0, used = 0, stars = 0;
    int weights[PAGE_MAX_FRAMES] = {0};
    if (spec && *spec)
        while (*spec && count < PAGE_MAX_FRAMES) {
            while (isspace((unsigned char)*spec))
                spec++;
            char *end;
            long value = strtol(spec, &end, 10);
            if (end == spec)
                value = 1;
            if (value < 0)
                value = 0;
            if (value > 10000)
                value = 10000;
            if (*end == '*' || end == spec) {
                weights[count] = (int)value;
                stars += (int)value;
                out[count] = 0;
            } else {
                out[count] = *end == '%' ? total * (int)value / 100 : CSS_TO_DEV((int)value);
                used += out[count];
            }
            count++;
            spec = strchr(end, ',');
            if (!spec)
                break;
            spec++;
        }
    if (!count) {
        count = fallback > 0 ? fallback : 1;
        if (count > PAGE_MAX_FRAMES)
            count = PAGE_MAX_FRAMES;
        for (int i = 0; i < count; i++)
            out[i] = total / count;
    } else if (stars && used < total)
        for (int i = 0; i < count; i++)
            out[i] += (total - used) * weights[i] / stars;
    int sum = 0;
    for (int i = 0; i < count; i++)
        sum += out[i];
    if (sum > total)
        for (int i = 0; i < count; i++)
            out[i] = (int)((int64_t)out[i] * total / sum);
    sum = 0;
    for (int i = 0; i < count; i++)
        sum += out[i];
    out[count - 1] += total - sum;
    return count;
}
static void emit_set(struct page *p, struct node *set, int x, int y, int w, int h, int depth)
{
    if (depth > FRAME_MAX_DEPTH || w < 1 || h < 1)
        return;
    struct node *children[PAGE_MAX_FRAMES];
    int nc = 0;
    for (struct node *c = set->first; c && nc < PAGE_MAX_FRAMES; c = c->next)
        if (c->tag == T_FRAME || c->tag == T_FRAMESET)
            children[nc++] = c;
    int rows[PAGE_MAX_FRAMES], cols[PAGE_MAX_FRAMES];
    const char *r = node_attr(set, "rows"), *c = node_attr(set, "cols");
    int nr = tracks(r, h, c ? 1 : nc, rows), ncol = tracks(c, w, 1, cols);
    int at = 0, cy = y;
    for (int ry = 0; ry < nr; ry++) {
        int cx = x;
        for (int co = 0; co < ncol && at < nc; co++, at++) {
            struct node *n = children[at];
            if (n->tag == T_FRAMESET)
                emit_set(p, n, cx, cy, cols[co], rows[ry], depth + 1);
            else if (n->frame >= 0 && n->frame < p->nframes && p->nitems < p->item_cap) {
                struct ditem *d = &p->items[p->nitems++];
                memset(d, 0, sizeof *d);
                d->kind = DI_FRAME;
                d->node = n;
                d->x = cx;
                d->y = cy;
                d->w = cols[co];
                d->h = rows[ry];
                d->u.frame = &p->frames[n->frame];
                frame_geometry(d->u.frame, cx + 1, cy + 1, d->w > 2 ? d->w - 2 : 1,
                               d->h > 2 ? d->h - 2 : 1);
            }
            cx += cols[co];
        }
        cy += rows[ry];
    }
}
bool frameset_layout(struct page *p, int width)
{
    struct node *set = NULL;
    for (struct node *n = p->body ? p->body->first : NULL; n; n = n->next)
        if (n->tag == T_FRAMESET) {
            set = n;
            break;
        }
    if (!set)
        return false;
    if (p->item_cap < PAGE_MAX_FRAMES) {
        struct ditem *d = realloc(p->items, PAGE_MAX_FRAMES * sizeof *d);
        if (!d) {
            p->oom = true;
            return true;
        }
        p->items = d;
        p->item_cap = PAGE_MAX_FRAMES;
    }
    p->width = width;
    p->nitems = 0;
    p->content_h = p->viewport_h > 0 ? p->viewport_h : 226;
    emit_set(p, set, 0, 0, width, p->content_h, 0);
    return true;
}
struct frame_slot *page_frame_at(struct page *p, int x, int y)
{
    for (int i = p ? p->nitems - 1 : -1; i >= 0; i--) {
        struct ditem *d = &p->items[i];
        if (d->kind != DI_FRAME)
            continue;
        struct frame_slot *f = d->u.frame;
        if (x < f->x || y < f->y || x >= f->x + f->w || y >= f->y + f->h)
            continue;
        struct frame_slot *inner = page_frame_at(f->page, x - f->x, y - f->y + f->scroll);
        return inner ? inner : f;
    }
    return NULL;
}
