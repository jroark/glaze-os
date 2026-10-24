/* JerryScript integration. Each document owns a realm; the shared VM has a
 * fixed heap and a fatal-allocation boundary that leaves the browser usable. */
#include "script.h"
#include "frames.h"
#include "plat.h"
#include "url.h"
#include "js_platform/port.h"
#include "jerryscript.h"
#include "js_bootstrap.h"
#include "identity.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define JS_PAGES (PAGE_MAX_FRAMES + 1)
#define JS_NODES 1024
struct handle {
    struct node *node;
    uintptr_t id;
    jerry_value_t value;
};
struct runtime {
    struct page *page;
    jerry_value_t realm, proto, dispatch, tick;
    struct handle nodes[JS_NODES];
    int nnodes;
    struct node *current, *write_after;
    uint32_t next_tick;
};
static struct runtime *pages[JS_PAGES];
static const jerry_object_native_info_t node_info = {0}, fn_info = {0};
static bool running, live;
extern void glaze_js_stack_base(uintptr_t base);
static uint32_t deadline, checks;
static uintptr_t next_handle = 1;
/* Only buffers crossing an allocating engine API need this cleanup list. */
static void *temps[64];
static int ntemps;
static void *temporary(void *p)
{
    if (p && ntemps < 64)
        temps[ntemps++] = p;
    else if (p) {
        free(p);
        return NULL;
    }
    return p;
}
static void clean_temps(void)
{
    while (ntemps)
        free(temps[--ntemps]);
}
static void failed(void)
{
    clean_temps();
    js_failure_guard = running = false;
    js_port_discard();
    live = false;
    for (int i = 0; i < JS_PAGES; i++)
        if (pages[i]) {
            pages[i]->page->script_disabled = true;
            pages[i]->page->js = NULL;
            free(pages[i]);
            pages[i] = NULL;
        }
}
/* Also polled by Jerry's native regexp backtracker (which has no VM jumps). */
bool glaze_js_budget_expired(void)
{
    if (!running)
        return false;
    if (!checks || --checks == 0)
        return true;
    if ((checks & 63u) == 0 && (int32_t)(plat_ms() - deadline) >= 0) {
        checks = 0;
        return true;
    }
    return false;
}
static jerry_value_t halt(void *unused)
{
    (void)unused;
    return glaze_js_budget_expired() ? jerry_boolean(true) : jerry_undefined();
}
static void budget(void *base)
{
    if (live)
        glaze_js_stack_base((uintptr_t)base);
    deadline = plat_ms() + 150;
    checks = 12000;
    running = true;
}
static struct runtime *by_serial(uintptr_t id)
{
    for (int i = 0; i < JS_PAGES; i++)
        if (pages[i] && pages[i]->page->serial == id)
            return pages[i];
    return NULL;
}
static struct node *node_arg(jerry_value_t v)
{
    if (!jerry_value_is_object(v))
        return NULL;
    uintptr_t id = (uintptr_t)jerry_object_get_native_ptr(v, &node_info);
    if (!id)
        return NULL;
    for (int i = 0; i < JS_PAGES; i++)
        if (pages[i])
            for (int j = 0; j < pages[i]->nnodes; j++)
                if (pages[i]->nodes[j].id == id)
                    return pages[i]->nodes[j].node;
    return NULL;
}
static jerry_value_t wrap(struct node *n)
{
    if (!n || !n->owner || !n->owner->js)
        return jerry_null();
    struct runtime *s = n->owner->js;
    for (int i = 0; i < s->nnodes; i++)
        if (s->nodes[i].node == n)
            return jerry_value_copy(s->nodes[i].value);
    if (s->nnodes >= JS_NODES)
        return jerry_null();
    struct handle *h = &s->nodes[s->nnodes++];
    h->node = n;
    h->id = next_handle++;
    h->value = jerry_object();
    jerry_object_set_native_ptr(h->value, &node_info, (void *)h->id);
    if (jerry_value_is_object(s->proto))
        jerry_value_free(jerry_object_set_proto(h->value, s->proto));
    return jerry_value_copy(h->value);
}
static char *string_arg(jerry_value_t v)
{
    jerry_value_t str = jerry_value_to_string(v);
    if (jerry_value_is_exception(str)) {
        jerry_value_free(str);
        return NULL;
    }
    jerry_size_t size = jerry_string_size(str, JERRY_ENCODING_UTF8);
    if (size > 65536) {
        jerry_value_free(str);
        return NULL;
    }
    char *utf = malloc(size + 1);
    if (!utf) {
        jerry_value_free(str);
        return NULL;
    }
    jerry_string_to_buffer(str, JERRY_ENCODING_UTF8, (jerry_char_t *)utf, size);
    utf[size] = 0;
    jerry_value_free(str);
    int len;
    char *text = cs_decode(CS_UTF8, (uint8_t *)utf, (int)size, &len);
    free(utf);
    return temporary(text);
}
static jerry_value_t text_value(const char *text)
{
    if (!text)
        return jerry_null();
    size_t n = strlen(text), len = 0;
    unsigned char *utf = temporary(malloc(n * 2 + 1));
    if (!utf)
        return jerry_undefined();
    for (size_t i = 0; i < n; i++) {
        unsigned c = (unsigned char)text[i];
        if (c >= 128) {
            utf[len++] = (unsigned char)(0xc0 | (c >> 6));
            utf[len++] = (unsigned char)(0x80 | (c & 63));
        } else
            utf[len++] = (unsigned char)c;
    }
    return jerry_string(utf, (jerry_size_t)len, JERRY_ENCODING_UTF8);
}
static char *raw_text(struct node *root)
{
    size_t size = 0;
    for (struct node *n = root; n; n = node_next(n, root, false))
        if (n->type == N_TEXT) {
            if (n->len < 0 || size + (size_t)n->len > 65536)
                return NULL;
            size += (size_t)n->len;
        }
    char *text = temporary(malloc(size + 1));
    if (!text)
        return NULL;
    size = 0;
    for (struct node *n = root; n; n = node_next(n, root, false))
        if (n->type == N_TEXT) {
            memcpy(text + size, n->text, (size_t)n->len);
            size += (size_t)n->len;
        }
    text[size] = 0;
    return text;
}
/* A deliberately conservative subset of CSP source expressions. Unknown
 * expressions never grant permission. Multiple header policies deny scripts. */
static bool directive(const char *policy, const char *name, const char **start, size_t *len)
{
    if (!policy)
        return false;
    size_t nl = strlen(name);
    for (const char *p = policy; *p;) {
        while (*p == ';' || isspace((unsigned char)*p))
            p++;
        const char *e = strchr(p, ';');
        if (!e)
            e = p + strlen(p);
        if ((size_t)(e - p) >= nl && !strncasecmp(p, name, nl) &&
            (p + nl == e || isspace((unsigned char)p[nl]))) {
            *start = p + nl;
            *len = (size_t)(e - p - nl);
            return true;
        }
        p = e;
    }
    return false;
}
static bool token(const char *s, size_t n, const char *wanted)
{
    return n == strlen(wanted) && !strncmp(s, wanted, n);
}
static bool policy_allows(struct page *p, const char *source, const struct node *node,
                          const char *special)
{
    const char *s;
    size_t len;
    if (!directive(p->csp, "script-src", &s, &len) && !directive(p->csp, "default-src", &s, &len))
        return true;
    const char *end = s + len;
    bool nonce_or_hash = false, strict_dynamic = false;
    for (const char *t = s; t < end;) {
        while (t < end && isspace((unsigned char)*t))
            t++;
        const char *e = t;
        while (e < end && !isspace((unsigned char)*e))
            e++;
        if (e - t >= 7 && (!strncmp(t, "'nonce-", 7) || !strncmp(t, "'sha256-", 8) ||
                           !strncmp(t, "'sha384-", 8) || !strncmp(t, "'sha512-", 8)))
            nonce_or_hash = true;
        if (token(t, (size_t)(e - t), "'strict-dynamic'"))
            strict_dynamic = true;
        t = e;
    }
    for (; s < end;) {
        while (s < end && isspace((unsigned char)*s))
            s++;
        const char *e = s;
        while (e < end && !isspace((unsigned char)*e))
            e++;
        size_t n = (size_t)(e - s);
        if (special && token(s, n, special))
            return true;
        if (!source && !special && !nonce_or_hash && !strict_dynamic &&
            token(s, n, "'unsafe-inline'"))
            return true;
        const char *nonce = node ? node_attr(node, "nonce") : NULL;
        if (!special && nonce && *nonce && n == strlen(nonce) + 8 && !strncmp(s, "'nonce-", 7) &&
            s[n - 1] == '\'' && !strncmp(s + 7, nonce, n - 8))
            return true;
        if (source && !special && !strict_dynamic) {
            if (token(s, n, "*"))
                return !strncasecmp(source, "http:", 5) || !strncasecmp(source, "https:", 6);
            struct page target = {.origin = (char *)source};
            if (token(s, n, "'self'") && page_same_origin(p, &target))
                return true;
            if (n && s[n - 1] == ':' && strlen(source) >= n && !strncasecmp(s, source, n))
                return true;
            if (n && s[0] != '\'' && n < 512) {
                char origin[512];
                memcpy(origin, s, n);
                origin[n] = 0;
                struct page allowed = {.origin = origin};
                if (page_same_origin(&allowed, &target)) {
                    struct url u;
                    if (url_parse(origin, &u) && (!u.path[0] || !strcmp(u.path, "/")))
                        return true;
                }
            }
        }
        s = e;
    }
    return false;
}
bool script_allowed(struct page *p, const char *source, const struct node *node)
{
    return p && p->scripting_enabled && !p->script_disabled &&
           (!source || page_resource_allowed(p, source)) &&
           policy_allows(p, source, node, NULL);
}
static jerry_value_t window_value(struct page *caller, struct page *target)
{
    if (!target || !target->js || !page_same_origin(caller, target))
        return jerry_null();
    return jerry_value_copy(((struct runtime *)target->js)->realm);
}
static jerry_value_t native(const jerry_call_info_t *info, const jerry_value_t args[],
                            const jerry_length_t count)
{
    struct runtime *s = by_serial((uintptr_t)jerry_object_get_native_ptr(info->function, &fn_info));
    if (!s || !count)
        return jerry_undefined();
    int base = ntemps;
    char *op = string_arg(args[0]);
    if (!op)
        return jerry_undefined();
    struct page *p = s->page;
    struct node *n = count > 1 ? node_arg(args[1]) : NULL;
    if (n && !page_same_origin(p, n->owner)) {
        while (ntemps > base)
            free(temps[--ntemps]);
        return jerry_null();
    }
    struct page *owner = n ? n->owner : p;
    jerry_value_t a = count > 2 ? args[2] : jerry_undefined(),
                  b = count > 3 ? args[3] : jerry_undefined();
    jerry_value_t result = jerry_undefined();
    char *str = NULL;
#define IS(x) (!strcmp(op, x))
#define TEXT(v)                                                                                    \
    do {                                                                                           \
        result = text_value(v);                                                                    \
    } while (0)
    if (IS("document"))
        result = wrap(owner->doc);
    else if (IS("now"))
        result = jerry_number(plat_ms());
    else if (IS("evalAllowed"))
        result = jerry_boolean(policy_allows(p, NULL, NULL, "'unsafe-eval'"));
    else if (IS("userAgent"))
        TEXT(BROWSER_USER_AGENT);
    else if (IS("language"))
        TEXT(BROWSER_LANGUAGE);
    else if (IS("log")) {
        str = string_arg(a);
        if (str)
            printf("browser JS: %s\n", str);
    } else if (IS("URL"))
        TEXT(owner->url);
    else if (IS("navigate")) {
        str = string_arg(a);
        if (str)
            page_navigate(p, str, "_self", NULL);
    } else if (IS("parent") && !n)
        result = window_value(p, p->parent ? p->parent : p);
    else if (IS("top"))
        result = window_value(p, page_root(p));
    else if (IS("length"))
        result = jerry_number(p->nframes);
    else if (IS("frames")) {
        result = jerry_array(0);
        for (int i = 0; i < p->nframes; i++) {
            jerry_value_t w = window_value(p, p->frames[i].page);
            jerry_value_free(jerry_object_set_index(result, (uint32_t)i, w));
            const char *name = node_attr(p->frames[i].node, "name");
            if (name && *name && strcmp(name, "__proto__") && strcmp(name, "length"))
                jerry_value_free(jerry_object_set_sz(result, name, w));
            jerry_value_free(w);
        }
    } else if (n) {
        if (IS("type"))
            result = jerry_number(n->type == N_DOC ? 9 : n->type == N_TEXT ? 3 : 1);
        else if (IS("name")) {
            char name[64];
            snprintf(name, sizeof name, "%s",
                     n->type == N_DOC    ? "#document"
                     : n->type == N_TEXT ? "#text"
                                         : n->name);
            if (n->type == N_ELEM)
                for (char *c = name; *c; c++)
                    *c = (char)toupper((unsigned char)*c);
            TEXT(name);
        } else if (IS("parent"))
            result = wrap(n->parent);
        else if (IS("first"))
            result = wrap(n->first);
        else if (IS("last"))
            result = wrap(n->last);
        else if (IS("next"))
            result = wrap(n->next);
        else if (IS("prev"))
            result = wrap(n->prev);
        else if (IS("connected"))
            result = jerry_boolean(node_connected(n));
        else if (IS("children")) {
            result = jerry_array(0);
            unsigned i = 0;
            for (struct node *c = n->first; c && i < JS_NODES; c = c->next) {
                jerry_value_t v = wrap(c);
                jerry_value_free(jerry_object_set_index(result, i++, v));
                jerry_value_free(v);
            }
        } else if (IS("text"))
            TEXT(raw_text(n));
        else if (IS("html") || IS("outer"))
            TEXT(temporary(dom_serialize(owner, n, IS("outer"))));
        else if (IS("text=") || IS("html=")) {
            str = string_arg(a);
            if (str) {
                dom_content(owner, n, str, IS("html="));
                if (IS("html="))
                    for (struct node *c = n->first; c; c = node_next(c, n, false))
                        if (c->tag == T_SCRIPT)
                            c->collected |= 4;
            }
        } else if (IS("attr")) {
            str = string_arg(a);
            if (str) {
                for (char *c = str; *c; c++)
                    *c = (char)tolower((unsigned char)*c);
                TEXT(node_attr(n, str));
            }
        } else if (IS("attr=") || IS("attr-")) {
            str = string_arg(a);
            char *val = IS("attr=") ? string_arg(b) : NULL;
            if (str)
                dom_attr(owner, n, str, val);
        } else if (IS("create") || IS("createText")) {
            str = string_arg(a);
            if (str)
                result = wrap(dom_create(owner, IS("create") ? N_ELEM : N_TEXT,
                                         IS("create") ? str : NULL, IS("createText") ? str : NULL));
        } else if (IS("insert")) {
            struct node *child = node_arg(a), *before = node_arg(b);
            if (dom_insert(owner, n, child, before))
                result = wrap(child);
        } else if (IS("remove"))
            dom_remove(owner, n);
        else if (IS("query")) {
            str = string_arg(a);
            struct node *nodes[256];
            int num = str ? dom_select(owner, n, str, nodes, 256) : 0;
            result = jerry_array(0);
            for (int i = 0; i < num; i++) {
                jerry_value_t v = wrap(nodes[i]);
                jerry_value_free(jerry_object_set_index(result, (uint32_t)i, v));
                jerry_value_free(v);
            }
        } else if (IS("id")) {
            str = string_arg(a);
            result = jerry_null();
            for (struct node *c = owner->doc; str && c; c = node_next(c, owner->doc, false)) {
                const char *id = node_attr(c, "id");
                if (id && !strcmp(id, str)) {
                    result = wrap(c);
                    break;
                }
            }
        } else if (IS("body"))
            result = wrap(owner->body);
        else if (IS("head")) {
            result = jerry_null();
            for (struct node *c = owner->html ? owner->html->first : NULL; c; c = c->next)
                if (c->tag == T_HEAD) {
                    result = wrap(c);
                    break;
                }
        } else if (IS("documentElement"))
            result = wrap(owner->html);
        else if (IS("currentScript"))
            result = wrap(((struct runtime *)owner->js)->current);
        else if (IS("readyState"))
            TEXT(owner->load_event ? "complete" : owner->ready_event ? "interactive" : "loading");
        else if (IS("title"))
            TEXT(owner->title ? owner->title : "");
        else if (IS("title=")) {
            str = string_arg(a);
            if (str) {
                owner->title = arena_strndup(&owner->arena, str, (int)strlen(str));
                struct node *title = dom_query(owner, owner->doc, "title", NULL);
                if (!title) {
                    struct node *head = dom_query(owner, owner->doc, "head", NULL);
                    title = dom_create(owner, N_ELEM, "title", NULL);
                    if (head && title)
                        dom_insert(owner, head, title, NULL);
                }
                if (title)
                    dom_content(owner, title, str, false);
                owner->dirty = true;
            }
        } else if (IS("value"))
            TEXT(n->ctl ? n->ctl->value : node_attr(n, "value"));
        else if (IS("value=")) {
            str = string_arg(a);
            if (str)
                dom_attr(owner, n, "value", str);
        } else if (IS("checked") || IS("disabled"))
            result = jerry_boolean(n->ctl ? (IS("checked") ? n->ctl->checked : n->ctl->disabled)
                                          : node_attr(n, op) != NULL);
        else if (IS("checked=") || IS("disabled="))
            dom_attr(owner, n, IS("checked=") ? "checked" : "disabled",
                     jerry_value_to_boolean(a) ? "" : NULL);
        else if (IS("selectedIndex"))
            result = jerry_number(n->ctl ? n->ctl->sel : 0);
        else if (IS("selectedIndex=")) {
            if (n->ctl && jerry_value_is_number(a)) {
                int v = (int)jerry_value_as_number(a);
                if (v >= 0 && v < n->ctl->nopts) {
                    n->ctl->sel = v;
                    owner->dirty = true;
                }
            }
        } else if (IS("submit")) {
            char *post = NULL, *url = forms_submit(owner, n, NULL, &post);
            if (url) {
                page_navigate(owner, url, node_attr(n, "target"), post);
                free(url);
            }
            free(post);
        } else if (IS("reset")) {
            forms_reset(owner, n);
            owner->dirty = true;
        } else if (IS("frameDocument") || IS("frameWindow")) {
            result = jerry_null();
            if (n->frame >= 0 && n->frame < owner->nframes) {
                struct page *child = owner->frames[n->frame].page;
                if (child && page_same_origin(p, child))
                    result = IS("frameDocument") ? wrap(child->doc) : window_value(p, child);
            }
        } else if (IS("handler")) {
            str = string_arg(a);
            if (str && script_allowed(owner, NULL, NULL)) {
                size_t len = strlen(str) + 40;
                char *code = temporary(malloc(len));
                if (code) {
                    snprintf(code, len, "(function(event){%s\n})", str);
                    result = jerry_eval((jerry_char_t *)code, strlen(code), JERRY_PARSE_NO_OPTS);
                }
            }
        } else if (IS("write")) {
            str = string_arg(a);
            if (str) {
                struct node *tmp = dom_create(owner, N_ELEM, "div", NULL);
                if (tmp && dom_content(owner, tmp, str, true)) {
                    struct node *after = s->write_after ? s->write_after : s->current,
                                *parent = after ? after->parent : owner->body,
                                *before = after ? after->next : NULL;
                    while (tmp->first) {
                        struct node *child = tmp->first;
                        if (!dom_insert(owner, parent, child, before))
                            break;
                        s->write_after = child;
                    }
                }
            }
        }
    }
#undef IS
#undef TEXT
    while (ntemps > base)
        free(temps[--ntemps]);
    return result;
}
static void report(jerry_value_t v)
{
    if (jerry_value_is_exception(v)) {
        jerry_value_t e = jerry_exception_value(v, false);
        char *message = string_arg(e);
        printf("browser JS: %s\n", message ? message : "script interrupted");
        jerry_value_free(e);
    }
    jerry_value_free(v);
}
bool script_init(struct page *p)
{
    if (!p || !p->scripting_enabled || p->script_disabled)
        return false;
    if (p->js)
        return true;
    if (js_failure_guard)
        return false;
    js_failure_guard = true;
    if (setjmp(js_failure_boundary)) {
        failed();
        p->script_disabled = true;
        return false;
    }
    budget(__builtin_frame_address(0));
    if (!live) {
        jerry_init(JERRY_INIT_EMPTY);
        live = true;
        glaze_js_stack_base((uintptr_t)__builtin_frame_address(0));
        jerry_halt_handler(128, halt, NULL);
    }
    int slot = 0;
    while (slot < JS_PAGES && pages[slot])
        slot++;
    if (slot == JS_PAGES) {
        js_failure_guard = running = false;
        return false;
    }
    struct runtime *s = calloc(1, sizeof *s);
    if (!s) {
        js_failure_guard = running = false;
        return false;
    }
    s->page = p;
    s->proto = s->dispatch = s->tick = jerry_undefined();
    p->js = s;
    pages[slot] = s;
    s->realm = jerry_realm();
    jerry_value_t old = jerry_set_realm(s->realm);
    jerry_value_t fn = jerry_function_external(native);
    jerry_object_set_native_ptr(fn, &fn_info, (void *)(uintptr_t)p->serial);
    jerry_value_free(jerry_object_set_sz(s->realm, "__dom", fn));
    jerry_value_free(fn);
    jerry_value_t api = jerry_eval((const jerry_char_t *)js_bootstrap, sizeof(js_bootstrap) - 1,
                                   JERRY_PARSE_NO_OPTS);
    bool ok = !jerry_value_is_exception(api);
    if (ok) {
        s->proto = jerry_object_get_sz(api, "proto");
        s->dispatch = jerry_object_get_sz(api, "dispatch");
        s->tick = jerry_object_get_sz(api, "tick");
        for (int i = 0; i < s->nnodes; i++)
            jerry_value_free(jerry_object_set_proto(s->nodes[i].value, s->proto));
    }
    report(api);
    (void)jerry_set_realm(old);
    clean_temps();
    js_failure_guard = running = false;
    if (!ok) {
        p->script_disabled = true;
        script_free(p);
    }
    return ok;
}
void script_free(struct page *p)
{
    if (!p || !p->js)
        return;
    struct runtime *s = p->js;
    p->js = NULL;
    for (int i = 0; i < JS_PAGES; i++)
        if (pages[i] == s)
            pages[i] = NULL;
    if (live) {
        for (int i = 0; i < s->nnodes; i++)
            jerry_value_free(s->nodes[i].value);
        jerry_value_free(s->proto);
        jerry_value_free(s->dispatch);
        jerry_value_free(s->tick);
        jerry_value_free(s->realm);
    }
    free(s);
}
bool script_eval(struct page *p, const char *code, int len, struct node *current)
{
    if (!script_init(p) || !code || len < 0 || len > 131072)
        return false;
    js_failure_guard = true;
    if (setjmp(js_failure_boundary)) {
        failed();
        return false;
    }
    budget(__builtin_frame_address(0));
    struct runtime *s = p->js;
    s->current = current;
    s->write_after = NULL;
    jerry_value_t old = jerry_set_realm(s->realm);
    jerry_value_t result = jerry_eval((const jerry_char_t *)code, (size_t)len, JERRY_PARSE_NO_OPTS);
    bool ok = !jerry_value_is_exception(result);
    report(result);
    jerry_value_t jobs = jerry_run_jobs();
    ok = ok && !jerry_value_is_exception(jobs);
    report(jobs);
    s->current = s->write_after = NULL;
    (void)jerry_set_realm(old);
    clean_temps();
    js_failure_guard = running = false;
    return ok;
}
bool script_event(struct page *p, struct node *target, const char *type)
{
    if (!script_init(p))
        return true;
    js_failure_guard = true;
    if (setjmp(js_failure_boundary)) {
        failed();
        return true;
    }
    budget(__builtin_frame_address(0));
    struct runtime *s = p->js;
    jerry_value_t old = jerry_set_realm(s->realm);
    jerry_value_t args[2] = {target ? wrap(target) : jerry_value_copy(s->realm), text_value(type)};
    jerry_value_t v = jerry_call(s->dispatch, jerry_undefined(), args, 2);
    bool allow = jerry_value_is_exception(v) || jerry_value_to_boolean(v);
    report(v);
    report(jerry_run_jobs());
    jerry_value_free(args[0]);
    jerry_value_free(args[1]);
    (void)jerry_set_realm(old);
    clean_temps();
    js_failure_guard = running = false;
    return allow;
}
void script_tick(struct page *p)
{
    if (!p || !p->js || p->script_disabled)
        return;
    struct runtime *s = p->js;
    uint32_t now = plat_ms();
    if ((int32_t)(now - s->next_tick) < 0)
        return;
    s->next_tick = now + 16;
    js_failure_guard = true;
    if (setjmp(js_failure_boundary)) {
        failed();
        return;
    }
    budget(__builtin_frame_address(0));
    jerry_value_t old = jerry_set_realm(s->realm);
    report(jerry_call(s->tick, s->realm, NULL, 0));
    report(jerry_run_jobs());
    (void)jerry_set_realm(old);
    clean_temps();
    js_failure_guard = running = false;
}
void script_shutdown(void)
{
    for (int i = 0; i < JS_PAGES; i++)
        if (pages[i])
            script_free(pages[i]->page);
    if (live)
        jerry_cleanup();
    live = false;
}
