/* Opt-in live smoke test using the real engine, forms and navigation path. */
#include "../apps/browser/web.h"
#include "../apps/browser/load.h"
#include "../apps/browser/net.h"
#include "../apps/browser/script.h"
#include "../apps/browser/plat.h"
#include "../apps/browser/url.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void navigate(struct load *ld, const char *url, const char *post)
{
    struct page *old = ld->shown;
    load_begin(ld, url, post, NULL, false, 240);
    page_free(old);
}
static struct node *result_link(struct page *p, const char *host)
{
    for (struct node *n = p->doc; n; n = node_next(n, p->doc, false)) {
        if (n->tag != T_A)
            continue;
        char *url = page_link_url(p, n);
        struct url parsed;
        bool matches = url && url_parse(url, &parsed) &&
            (!strcmp(parsed.host, host) ||
             ((!strcmp(parsed.host, "www.google.com") || !strcmp(parsed.host, "google.com")) &&
              !strcmp(parsed.path, "/url") && strstr(parsed.query, host)));
        free(url);
        if (matches)
            return n;
    }
    return NULL;
}
int main(int argc, char **argv)
{
    if (argc > 3 || (argc > 1 && !strcmp(argv[1], "--help"))) {
        puts("usage: browser-google [query [expected-result-host]]\n"
             "defaults: 'example domain', example.com; exit 3 means CAPTCHA required");
        return argc > 3 ? 2 : 0;
    }
    const char *query = argc > 1 ? argv[1] : "example domain";
    const char *host = argc > 2 ? argv[2] : "example.com";
    struct load ld = {0};
    uint32_t start = plat_ms(), idle = 0, refresh_at = 0;
    char *refresh_url = NULL;
    int phase = 0, pages = 0, status = 1;
    setbuf(stdout, NULL);
    navigate(&ld, "https://www.google.com/", NULL);
    while (plat_ms() - start < 90000 && pages < 12) {
        int flags = load_step(&ld);
        struct page *p = ld.shown;
        if (flags & LF_NEW_PAGE) {
            printf("Google smoke: %s — %s\n", p->url, p->title ? p->title : "");
            pages++;
            idle = 0;
            free(refresh_url);
            refresh_url = NULL;
            int delay;
            const char *refresh = page_refresh(p, &delay);
            if (refresh && delay >= 0 && delay <= 10) {
                refresh_url = url_resolve(p->base, refresh);
                refresh_at = plat_ms() + (unsigned)delay * 1000;
            }
        }
        if (!p) {
            usleep(1000);
            continue;
        }
        if (strstr(p->url, "/sorry/") || dom_query(p, p->doc, "#captcha-form", NULL)) {
            puts("Google smoke: BLOCKED — Google requires human verification for this network");
            status = 3;
            break;
        }
        if (p->is_error) {
            puts("Google smoke: FAIL — document load failed");
            break;
        }
        if (refresh_url && ld.state == LS_IDLE && (int32_t)(plat_ms() - refresh_at) >= 0) {
            page_navigate(p, refresh_url, "_self", NULL);
            free(refresh_url);
            refresh_url = NULL;
        }
        if (p->navigate_url) {
            char *url = strdup(p->navigate_url);
            char *post = p->navigate_post ? strdup(p->navigate_post) : NULL;
            if (!url || (p->navigate_post && !post)) {
                free(url); free(post);
                break;
            }
            navigate(&ld, url, post);
            free(url); free(post);
            idle = 0;
            continue;
        }
        if (ld.state != LS_IDLE || ld_busy()) {
            idle = 0;
            usleep(1000);
            continue;
        }
        if (!idle)
            idle = plat_ms();
        if (plat_ms() - idle < 2000) {
            usleep(1000);
            continue;
        }
        if (phase == 0) {
            struct ctl *field;
            for (field = p->ctls; field; field = field->next) {
                const char *name = node_attr(field->node, "name");
                if (name && !strcmp(name, "q") && !field->disabled && field->form &&
                    (field->type == C_TEXT || field->type == C_TEXTAREA))
                    break;
            }
            if (!field || strlen(query) >= (size_t)field->cap) {
                puts("Google smoke: FAIL — search field unavailable or query too long");
                break;
            }
            snprintf(field->value, (size_t)field->cap, "%s", query);
            field->len = (int)strlen(query);
            if (script_event(p, field->form, "submit")) {
                char *post = NULL, *url = forms_submit(p, field->form, NULL, &post);
                if (url)
                    page_navigate(p, url, "_self", post);
                free(url); free(post);
            }
            phase = 1;
            idle = 0;
        } else if (phase == 1) {
            struct node *link = result_link(p, host);
            if (!link) {
                puts("Google smoke: FAIL — expected result link unavailable");
                break;
            }
            if (script_event(p, link, "click")) {
                char *url = page_link_url(p, link);
                if (url)
                    page_navigate(p, url, "_self", NULL);
                free(url);
            }
            phase = 2;
            idle = 0;
        } else {
            struct url parsed;
            if (url_parse(p->url, &parsed) && !strcmp(parsed.host, host) && p->nitems > 0) {
                puts("Google smoke: PASS — home, query, results and result navigation");
                status = 0;
            } else {
                puts("Google smoke: FAIL — result did not reach the expected host");
            }
            break;
        }
    }
    if (plat_ms() - start >= 90000 || pages >= 12)
        puts("Google smoke: FAIL — time or navigation limit exceeded");
    struct page *p = ld.shown;
    load_stop(&ld);
    load_forget(&ld, p);
    page_free(p);
    free(ld.url);
    free(refresh_url);
    script_shutdown();
    return status;
}
