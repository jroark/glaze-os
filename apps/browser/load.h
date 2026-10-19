/*
 * One page load: the document, then its stylesheets (waited for up to
 * CSS_WAIT_MS before the first layout, so pages don't flash unstyled),
 * then its images. The old page stays up until the new one is laid out.
 */
#ifndef BROWSER_LOAD_H
#define BROWSER_LOAD_H

#include "web.h"

enum { LS_IDLE, LS_DOC, LS_STYLE, LS_IMAGES };

/* load_step() flags. */
#define LF_NEW_PAGE 1           /* ld->page is new: show it (the caller owns it) */
#define LF_LAYOUT   2           /* the page was laid out again */
#define LF_PAINT    4           /* repaint (an image arrived) */
#define LF_DONE     8           /* nothing left to load */

struct load {
    int state;
    int doc_id;
    int width;
    bool reload;
    char *url;                  /* requested */
    struct page *page;          /* being built, until LF_NEW_PAGE */
    struct page *shown;         /* handed over; still fed images and CSS */
    uint32_t css_deadline, last_layout;
    bool layout_pending, restyle_pending;
    int pending_css, pending_img, total_img;
};

void load_begin(struct load *ld, const char *url, const char *post, const char *post_type, bool reload, int width);
int load_step(struct load *ld);
void load_stop(struct load *ld);
/* 0..1000 while loading. */
int load_progress(const struct load *ld);
/* The shown page went away (the caller freed it). */
void load_forget(struct load *ld, struct page *p);
/* An error page for url. */
struct page *load_error_page(const char *url, int err);

#endif
