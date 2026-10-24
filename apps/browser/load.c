/* Page loads (see load.h). */

#include "load.h"
#include "net.h"
#include "plat.h"
#include "script.h"
#include "frames.h"
#include "url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CSS_WAIT_MS 3000

static void escape(const char *s, char *out, int max)
{
    int o = 0;

    for (; *s && o < max - 6; s++) {
        if (*s == '<')
            o += snprintf(out + o, (size_t)(max - o), "&lt;");
        else if (*s == '&')
            o += snprintf(out + o, (size_t)(max - o), "&amp;");
        else if (*s == '"')
            o += snprintf(out + o, (size_t)(max - o), "&quot;");
        else
            out[o++] = *s;
    }
    out[o] = 0;
}

struct page *load_error_page(const char *url, int err)
{
    static char html[1536];
    char esc[600];
    struct page *p = page_new(url);

    if (!p)
        return NULL;
    escape(url, esc, sizeof(esc));
    snprintf(html, sizeof(html),
             "<html><head><title>Cannot Open Page</title><style>"
             "body{margin:28px 18px;background:#F2F3F6;color:#3A3F4A}"
             "h1{font-size:21px;color:#4C566C;margin:0 0 12px}p{font-size:15px;line-height:1.35}"
             ".u{color:#8A8F99;font-size:12px}</style></head><body>"
             "<h1>Cannot Open Page</h1><p>%s</p><p class=u>%s</p></body></html>",
             web_strerror(err), esc);
    p->is_error = true;
    page_set_html(p, html);
    page_style(p);
    return p;
}

static void stopped_tree(struct page *p)
{
    if (!p)
        return;
    for (int i = 0; i < p->nsheets; i++) {
        p->sheets[i].pending = false;
        p->sheets[i].fetch = 0;
    }
    for (int i = 0; i < p->nscripts; i++)
        if (p->scripts[i].state == RES_WAIT || p->scripts[i].state == RES_LOADING) {
            p->scripts[i].state = RES_FAILED;
            p->scripts[i].fetch = 0;
        }
    for (int i = 0; i < p->nimgs; i++)
        if (p->imgs[i].state == IMG_WAIT || p->imgs[i].state == IMG_LOADING) {
            p->imgs[i].state = IMG_FAILED;
            p->imgs[i].fetch = 0;
        }
    for (int i = 0; i < p->nframes; i++) {
        struct frame_slot *f = &p->frames[i];
        if (f->state == RES_WAIT || f->state == RES_LOADING) {
            f->state = RES_FAILED;
            f->fetch = 0;
        }
        stopped_tree(f->page);
    }
}

static void stop_fetches(struct load *ld)
{
    ld_cancel_all();
    stopped_tree(ld->shown);
    ld->doc_id = 0;
}

void load_stop(struct load *ld)
{
    stop_fetches(ld);
    page_free(ld->page);
    ld->page = NULL;
    ld->state = LS_IDLE;
}

void load_forget(struct load *ld, struct page *p)
{
    if (ld->shown == p) {
        ld->shown = NULL;
        if (ld->state == LS_IMAGES)
            load_stop(ld);
    }
}

static void retire_scripts(struct page *p)
{
    if (!p)
        return;
    for (int i = 0; i < p->nframes; i++)
        retire_scripts(p->frames[i].page);
    script_free(p);
    p->script_disabled = true;
}

void load_begin(struct load *ld, const char *url, const char *post, const char *post_type,
                bool reload, int width)
{
    load_stop(ld);
    retire_scripts(ld->shown);
    free(ld->url);
    ld->url = strdup(url);
    ld->width = width;
    ld->reload = reload;
    ld->shown = NULL;
    ld->state = LS_DOC;
    if (!strncasecmp(url, "about:", 6)) {
        ld->doc_id = 0; /* answered by load_step */
        return;
    }
    ld->doc_id = ld_start(url, F_DOC, 0, post, post_type, reload);
}

/* All documents share the bounded fetch queue. Fetch IDs, not per-document
 * slot numbers, route replies so child navigation cannot consume stale data. */
static struct page *document(struct fetch_result *r, const char *url)
{
    struct page *p;
    if (r->err || (r->status >= 400 && !r->len))
        return load_error_page(url, r->err ? r->err : WE_NOTFOUND);
    p = page_new(r->url ? r->url : url);
    if (p) {
        p->csp = r->csp ? strdup(r->csp) : NULL;
        page_set_document(p, r->data, r->len, r->ctype);
    }
    if (!p || p->oom) {
        page_free(p);
        p = load_error_page(url, WE_NOMEM);
    }
    return p;
}
static void got_document(struct load *ld, struct fetch_result *r)
{
    ld->doc_id = 0;
    ld->page = document(r, ld->url);
    ld->state = LS_STYLE;
    ld->css_deadline = plat_ms() + CSS_WAIT_MS;
}
static void attach(struct frame_slot *f, struct page *p, bool inherit)
{
    f->page = p;
    f->state = p ? RES_READY : RES_FAILED;
    f->fetch = 0;
    if (!p)
        return;
    p->parent = f->owner;
    p->host = f;
    p->script_disabled = f->sandboxed;
    p->scripting_enabled = !f->sandboxed;
    p->opaque_origin = f->sandboxed || !strncasecmp(p->url, "data:", 5);
    if (inherit) {
        free(p->origin);
        p->origin = strdup(f->owner->origin);
        free(p->base);
        p->base = strdup(f->owner->base);
        free(p->csp);
        p->csp = f->owner->csp ? strdup(f->owner->csp) : NULL;
        p->opaque_origin = p->opaque_origin || f->owner->opaque_origin;
    }
    /* The parent/depth and inherited policy must be known before collecting
     * nested resources or granting script permission. */
    if (p->doc)
        page_collect(p);
    p->dirty = true;
    f->owner->dirty = true;
}
static void local_frame(struct frame_slot *f, const char *srcdoc)
{
    struct page *p = page_new(srcdoc ? "about:srcdoc" : "about:blank");
    attach(f, p, true);
    if (p)
        page_set_html(p, srcdoc ? srcdoc : "");
}
static bool route(struct page *p, struct fetch_result *r)
{
    if (!p)
        return false;
    for (int i = 0; i < p->nsheets; i++)
        if (p->sheets[i].fetch == r->id) {
            struct sheet_slot *s = &p->sheets[i];
            s->fetch = 0;
            s->pending = false;
            if (!r->err && r->status < 400 && r->len && page_resource_allowed(p, r->url)) {
                int len;
                char *text = cs_decode(cs_sniff(r->ctype, r->data, r->len), r->data, r->len, &len);
                if (text) {
                    css_parse_sheet(&p->arena, &s->ss, text, len);
                    free(text);
                }
            }
            s->ss.loaded = node_connected(s->node);
            p->dirty = true;
            return true;
        }
    for (int i = 0; i < p->nimgs; i++)
        if (p->imgs[i].fetch == r->id) {
            struct image_slot *s = &p->imgs[i];
            s->fetch = 0;
            if (r->err || r->status >= 400 || !page_resource_allowed(p, r->url) ||
                !image_decode(p, s, r->data, r->len))
                s->state = IMG_FAILED;
            p->dirty = true;
            return true;
        }
    for (int i = 0; i < p->nscripts; i++)
        if (p->scripts[i].fetch == r->id) {
            struct script_slot *s = &p->scripts[i];
            s->fetch = 0;
            if (!r->err && r->status < 400 && r->len && script_allowed(p, r->url, s->node)) {
                s->source = malloc((size_t)r->len + 1);
                if (s->source) {
                    memcpy(s->source, r->data, (size_t)r->len);
                    s->source[r->len] = 0;
                    s->len = r->len;
                    s->state = RES_READY;
                    return true;
                }
            }
            s->state = RES_FAILED;
            return true;
        }
    for (int i = 0; i < p->nframes; i++) {
        struct frame_slot *f = &p->frames[i];
        if (f->fetch == r->id) {
            f->fetch = 0;
            if (!node_connected(f->node) || !page_resource_allowed(p, r->url) ||
                !frame_response_allowed(p, r->url, r->xfo, r->csp)) {
                f->state = RES_FAILED;
                return true;
            }
            struct page *child = page_new(r->url ? r->url : f->url);
            attach(f, child, false);
            if (child) {
                child->csp = r->csp ? strdup(r->csp) : NULL;
                if (r->err || (r->status >= 400 && !r->len))
                    page_set_html(child, "<p>Cannot open frame</p>");
                else
                    page_set_document(child, r->data, r->len, r->ctype);
            }
            return true;
        }
        if (route(f->page, r))
            return true;
    }
    return false;
}
static bool cyclic(struct page *p, const char *url)
{
    for (; p; p = p->parent)
        if (url_same_document(p->url, url))
            return true;
    return false;
}
static void schedule(struct page *p, int *flags)
{
    if (!p)
        return;
    if (p->resources_dirty) {
        /* A rescan may retire child contexts. Clear UI references before the
         * caller handles its next input event. */
        unsigned serials[PAGE_MAX_FRAMES] = {0};
        for (int i = 0; i < p->nframes; i++)
            serials[i] = p->frames[i].page ? p->frames[i].page->serial : 0;
        page_collect(p);
        for (int i = 0; i < p->nframes; i++)
            if (serials[i] != (p->frames[i].page ? p->frames[i].page->serial : 0))
                *flags |= LF_CONTEXT;
    }
    script_init(p);
    for (int i = 0; i < p->nsheets; i++) {
        struct sheet_slot *s = &p->sheets[i];
        if (!s->pending || s->fetch)
            continue;
        if (!node_connected(s->node) || !page_resource_allowed(p, s->url)) {
            s->pending = false;
            s->ss.loaded = true;
            continue;
        }
        if (ld_can_start())
            s->fetch = ld_start(s->url, F_CSS, i, NULL, NULL, false);
    }
    for (int i = 0; i < p->nscripts; i++) {
        struct script_slot *s = &p->scripts[i];
        if (s->state != RES_WAIT)
            continue;
        if (!node_connected(s->node) || !script_allowed(p, s->url, s->node)) {
            s->state = RES_DONE;
            continue;
        }
        if (ld_can_start()) {
            s->fetch = ld_start(s->url, F_SCRIPT, i, NULL, NULL, false);
            s->state = s->fetch > 0 ? RES_LOADING : RES_FAILED;
        }
    }
    for (int i = 0; i < p->nimgs; i++) {
        struct image_slot *s = &p->imgs[i];
        if (s->state != IMG_WAIT)
            continue;
        if (!page_resource_allowed(p, s->url)) {
            s->state = IMG_FAILED;
            continue;
        }
        if (ld_can_start()) {
            s->fetch = ld_start(s->url, F_IMG, i, NULL, NULL, false);
            s->state = s->fetch > 0 ? IMG_LOADING : IMG_FAILED;
        }
    }
    for (int i = 0; i < p->nframes; i++) {
        struct frame_slot *f = &p->frames[i];
        if (!node_connected(f->node)) {
            if (f->page || f->fetch) {
                *flags |= LF_CONTEXT;
                frame_cancel(f);
            }
            f->state = RES_DONE;
            continue;
        }
        if (f->page && f->page->navigate_url) {
            char *url = f->page->navigate_url, *post = f->page->navigate_post;
            f->page->navigate_url = f->page->navigate_post = NULL;
            frame_cancel(f);
            *flags |= LF_CONTEXT;
            free(f->url);
            free(f->post);
            f->url = url;
            f->post = post;
            f->state = RES_WAIT;
            f->inline_document = false;
        }
        if (f->state == RES_WAIT) {
            const char *srcdoc = node_attr(f->node, "srcdoc");
            if (srcdoc && f->inline_document)
                local_frame(f, srcdoc);
            else if (f->url && !strcasecmp(f->url, "about:blank"))
                local_frame(f, NULL);
            else if (!page_resource_allowed(p, f->url) || cyclic(p, f->url))
                f->state = RES_FAILED;
            else if (ld_can_start()) {
                f->fetch = ld_start(f->url, F_FRAME, i, f->post,
                                    f->post ? "application/x-www-form-urlencoded" : NULL, false);
                f->state = f->fetch > 0 ? RES_LOADING : RES_FAILED;
            }
        }
        schedule(f->page, flags);
    }
}
static bool css_pending(struct page *p)
{
    for (int i = 0; p && i < p->nsheets; i++)
        if (p->sheets[i].pending)
            return true;
    return false;
}
static char *inline_source(struct node *n, int *len)
{
    size_t size = 0;
    for (struct node *c = n->first; c; c = c->next)
        if (c->type == N_TEXT)
            size += (size_t)c->len;
    if (size > 131072)
        return NULL;
    char *text = malloc(size * 2 + 1);
    if (!text)
        return NULL;
    *len = 0;
    for (struct node *c = n->first; c; c = c->next)
        if (c->type == N_TEXT)
            for (int i = 0; i < c->len; i++) {
                unsigned v = (unsigned char)c->text[i];
                if (v >= 128) {
                    text[(*len)++] = (char)(0xc0 | (v >> 6));
                    text[(*len)++] = (char)(0x80 | (v & 63));
                } else
                    text[(*len)++] = (char)v;
            }
    text[*len] = 0;
    return text;
}
static bool scripts_pending(struct page *p)
{
    for (int i = 0; i < p->nscripts; i++)
        if (p->scripts[i].state != RES_DONE && node_connected(p->scripts[i].node))
            return true;
    return false;
}
static bool complete(struct page *p)
{
    if (!p || p->resources_dirty || scripts_pending(p) || css_pending(p))
        return false;
    for (int i = 0; i < p->nimgs; i++)
        if (p->imgs[i].state == IMG_WAIT || p->imgs[i].state == IMG_LOADING)
            return false;
    for (int i = 0; i < p->nframes; i++) {
        struct frame_slot *f = &p->frames[i];
        if (!node_connected(f->node))
            continue;
        if (f->state == RES_WAIT || f->state == RES_LOADING || (f->page && !f->page->load_event))
            return false;
    }
    return true;
}
static void execute(struct page *p, int *quota, int *flags, bool wait_css)
{
    if (!p)
        return;
    /* Classic scripts run in document order once the DOM has been parsed.
     * Rescanning after each edit also puts document.write scripts in order. */
    while (*quota > 0 && !(wait_css && css_pending(p))) {
        struct script_slot *s = NULL;
        for (struct node *n = p->doc; n && !s; n = node_next(n, p->doc, false))
            if (n->tag == T_SCRIPT)
                for (int i = 0; i < p->nscripts; i++)
                    if (p->scripts[i].node == n && p->scripts[i].state != RES_DONE) {
                        s = &p->scripts[i];
                        break;
                    }
        if (!s || s->state == RES_WAIT || s->state == RES_LOADING)
            break;
        bool allowed = script_allowed(p, s->url, s->node);
        s->state = s->state == RES_READY && allowed ? RES_READY : RES_FAILED;
        if (s->state == RES_READY) {
            int len = s->len;
            char *source = s->url ? s->source : inline_source(s->node, &len);
            if (source)
                script_eval(p, source, len, s->node);
            if (!s->url)
                free(source);
        }
        if (s->url)
            script_event(p, s->node, s->state == RES_READY ? "load" : "error");
        s->state = RES_DONE;
        free(s->source);
        s->source = NULL;
        (*quota)--;
        if (p->resources_dirty)
            schedule(p, flags);
    }
    if (!scripts_pending(p) && !p->ready_event) {
        p->ready_event = true;
        script_event(p, p->doc, "DOMContentLoaded");
    }
    for (int i = 0; i < p->nframes; i++) {
        struct frame_slot *f = &p->frames[i];
        execute(f->page, quota, flags, wait_css);
        if (!f->loaded_event && (f->state == RES_FAILED || (f->page && f->page->load_event))) {
            f->loaded_event = true;
            script_event(p, f->node, f->state == RES_FAILED ? "error" : "load");
        }
    }
    if (!p->load_event && complete(p)) {
        p->load_event = true;
        script_event(p, NULL, "load");
        /* HTML body onload is a window load handler. */
        if (p->body)
            script_event(p, p->body, "load");
    }
    script_tick(p);
}
static bool layout_tree(struct page *p, int width)
{
    if (!p)
        return false;
    bool dirty = p->dirty;
    for (int i = 0; i < p->nframes; i++)
        if (layout_tree(p->frames[i].page, p->frames[i].w > 0 ? p->frames[i].w : width))
            dirty = true;
    if (dirty) {
        page_style(p);
        page_layout(p, width);
        p->dirty = false;
    }
    return dirty;
}
int load_step(struct load *ld)
{
    int flags = 0;
    struct fetch_result r;
    if (ld->state == LS_DOC && !ld->doc_id) {
        struct page *p = page_new(ld->url);
        if (p && !strcasecmp(ld->url, "about:blank"))
            page_set_html(p, "");
        else {
            page_free(p);
            p = load_error_page(ld->url, WE_SCHEME);
        }
        ld->page = p;
        ld->state = LS_STYLE;
        ld->css_deadline = plat_ms();
    }
    ld_step(20);
    while (ld_take(&r)) {
        if (r.kind == F_DOC && r.id == ld->doc_id)
            got_document(ld, &r);
        else
            route(ld->page ? ld->page : ld->shown, &r);
        ld_free_result(&r);
    }
    struct page *p = ld->page ? ld->page : ld->shown;
    if (!p)
        return flags;
    schedule(p, &flags);
    int quota = 4;
    bool wait_css = (int32_t)(plat_ms() - ld->css_deadline) < 0;
    execute(p, &quota, &flags, wait_css);
    if (p->navigate_url)
        flags |= LF_NAVIGATE;
    if (ld->state == LS_STYLE && (!css_pending(p) || !wait_css)) {
        layout_tree(p, ld->width);
        ld->shown = p;
        ld->page = NULL;
        ld->state = LS_IMAGES;
        flags |= LF_NEW_PAGE;
    } else if (p == ld->shown && layout_tree(p, ld->width))
        flags |= LF_LAYOUT | LF_PAINT;
    if (ld->state == LS_IMAGES && complete(p)) {
        ld->state = LS_IDLE;
        flags |= LF_DONE;
    }
    return flags;
}
int load_progress(const struct load *ld)
{
    if (ld->state == LS_DOC) {
        int got, total;
        ld_progress(ld->doc_id, &got, &total);
        return 100 + (total ? 500 * got / total : got ? 300 : 0);
    }
    return ld->state == LS_STYLE ? 650 : ld->state == LS_IMAGES ? 850 : 0;
}
