/* Page loads (see load.h). */

#include "load.h"
#include "net.h"
#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CSS_WAIT_MS     3000
#define RELAYOUT_MS     700

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

static void stop_fetches(struct load *ld)
{
    ld_cancel_all();
    ld->doc_id = 0;
    ld->pending_css = ld->pending_img = 0;
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

void load_begin(struct load *ld, const char *url, const char *post, const char *post_type, bool reload, int width)
{
    load_stop(ld);
    free(ld->url);
    ld->url = strdup(url);
    ld->width = width;
    ld->reload = reload;
    ld->shown = NULL;
    ld->state = LS_DOC;
    if (!strncasecmp(url, "about:", 6)) {
        ld->doc_id = 0;         /* answered by load_step */
        return;
    }
    ld->doc_id = ld_start(url, F_DOC, 0, post, post_type, reload);
}

static void start_images(struct load *ld)
{
    struct page *p = ld->shown;

    for (int i = 0; i < p->nimgs; i++) {
        if (p->imgs[i].state != IMG_WAIT)
            continue;
        p->imgs[i].state = IMG_LOADING;
        p->imgs[i].fetch = ld_start(p->imgs[i].url, F_IMG, i, NULL, NULL, false);
        ld->pending_img++;
    }
    ld->total_img = ld->pending_img;
    ld->state = LS_IMAGES;
}

/* The document is in: build the page and ask for its stylesheets. */
static void got_document(struct load *ld, struct fetch_result *r)
{
    struct page *p;

    if (r->err || (r->status >= 400 && !r->len)) {
        p = load_error_page(ld->url, r->err ? r->err : WE_NOTFOUND);
    } else {
        p = page_new(r->url ? r->url : ld->url);
        if (p)
            page_set_document(p, r->data, r->len, r->ctype);
    }
    if (!p || p->oom) {
        page_free(p);
        p = load_error_page(ld->url, WE_NOMEM);
    }
    ld->page = p;
    ld->state = LS_STYLE;
    ld->css_deadline = plat_ms() + CSS_WAIT_MS;
    for (int i = 0; p && i < p->nsheets; i++) {
        if (!p->sheets[i].pending)
            continue;
        p->sheets[i].fetch = ld_start(p->sheets[i].url, F_CSS, i, NULL, NULL, false);
        ld->pending_css++;
    }
}

static void got_css(struct load *ld, struct fetch_result *r)
{
    struct page *p = ld->page ? ld->page : ld->shown;

    ld->pending_css--;
    if (!p || r->tag < 0 || r->tag >= p->nsheets || p->sheets[r->tag].fetch != r->id)
        return;
    struct sheet_slot *sl = &p->sheets[r->tag];
    sl->pending = false;
    if (!r->err && r->status < 400 && r->len) {
        int n;
        char *text = cs_decode(cs_sniff(r->ctype, r->data, r->len), r->data, r->len, &n);
        if (text) {
            css_parse_sheet(&p->arena, &sl->ss, text, n);
            /* Selectors and values point into the arena; the text can go. */
            free(text);
        }
    }
    sl->ss.loaded = true;
    if (p == ld->shown)
        ld->layout_pending = ld->restyle_pending = true;    /* after the first layout */
}

static void got_image(struct load *ld, struct fetch_result *r)
{
    struct page *p = ld->shown;

    ld->pending_img--;
    if (!p || r->tag < 0 || r->tag >= p->nimgs || p->imgs[r->tag].fetch != r->id)
        return;
    struct image_slot *s = &p->imgs[r->tag];
    if (r->err || r->status >= 400 || !image_decode(p, s, r->data, r->len)) {
        s->state = IMG_FAILED;
        ld->layout_pending = true;  /* alt text instead */
        return;
    }
    /* A size given by the attributes means nothing moves. */
    if (!s->want_w || !s->want_h)
        ld->layout_pending = true;
}

int load_step(struct load *ld)
{
    int flags = 0;
    struct fetch_result r;

    if (ld->state == LS_IDLE)
        return 0;
    if (ld->state == LS_DOC && !ld->doc_id) {
        /* about: pages */
        struct page *p = page_new(ld->url);
        if (p && !strcasecmp(ld->url, "about:blank"))
            page_set_html(p, "<html><body></body></html>");
        else {
            page_free(p);
            p = load_error_page(ld->url, WE_SCHEME);
        }
        ld->page = p;
        ld->state = LS_STYLE;
        ld->css_deadline = plat_ms();
    }
    ld_step(40);
    while (ld_take(&r)) {
        if (r.kind == F_DOC && r.id == ld->doc_id)
            got_document(ld, &r);
        else if (r.kind == F_CSS)
            got_css(ld, &r);
        else if (r.kind == F_IMG)
            got_image(ld, &r);
        ld_free_result(&r);
    }
    uint32_t now = plat_ms();
    if (ld->state == LS_STYLE && ld->page && (ld->pending_css <= 0 || (int32_t)(now - ld->css_deadline) >= 0)) {
        page_style(ld->page);
        page_layout(ld->page, ld->width);
        ld->shown = ld->page;
        ld->page = NULL;
        ld->last_layout = now;
        ld->layout_pending = false;
        flags |= LF_NEW_PAGE;
        start_images(ld);
    }
    if (ld->shown && ld->layout_pending &&
        ((int)(now - ld->last_layout) >= RELAYOUT_MS || (!ld->pending_img && ld->pending_css <= 0))) {
        if (ld->restyle_pending)
            page_style(ld->shown);
        page_layout(ld->shown, ld->width);
        ld->last_layout = now;
        ld->layout_pending = ld->restyle_pending = false;
        flags |= LF_LAYOUT;
    }
    if (ld->state == LS_IMAGES && !ld->pending_img && ld->pending_css <= 0 && !ld->layout_pending) {
        ld->state = LS_IDLE;
        flags |= LF_DONE;
    }
    if (ld->state == LS_IMAGES)
        flags |= LF_PAINT;
    return flags;
}

int load_progress(const struct load *ld)
{
    int got, total;

    switch (ld->state) {
    case LS_DOC:
        ld_progress(ld->doc_id, &got, &total);
        return 100 + (total ? 500 * got / total : (got ? 300 : 0));
    case LS_STYLE:
        return 650;
    case LS_IMAGES:
        return 700 + (ld->total_img ? 300 * (ld->total_img - ld->pending_img) / ld->total_img : 300);
    default:
        return 0;
    }
}
