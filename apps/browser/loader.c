/*
 * The loader: queues fetches, runs a few HTTP requests at once (the
 * document first, then stylesheets, then images), follows redirects, keeps
 * session cookies, and caches recent responses so going back and repeated
 * images don't refetch. Local schemes (res:, file:, data:) complete at
 * once.
 */

#include "net.h"
#include "plat.h"
#include "url.h"
#include "web.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_FETCHES     48
#define MAX_ACTIVE      4
#define MAX_REDIRECTS   8
#define CACHE_BYTES     (512 * 1024)
#define CACHE_ENTRY_MAX (192 * 1024)
#define CACHE_ENTRIES   48
#define MAX_COOKIES     64

static const int max_body[] = { [F_DOC] = 1024 * 1024, [F_FRAME]=64*1024, [F_SCRIPT]=128*1024, [F_CSS] = 384 * 1024, [F_IMG] = 768 * 1024 };

struct fetch {
    bool used, active, done, reload;
    uint8_t kind;
    int id, tag, redirects;
    char *url, *post, *post_type;
    int post_len;
    struct http *h;
    struct fetch_result r;
};

static struct fetch fetches[MAX_FETCHES];
static int next_id = 1;

/* ---- cookies ------------------------------------------------------------------- */

struct cookie {
    char *name, *value, *domain, *path;
    bool host_only, secure, http_only;
    long long expires;
};

static struct cookie jar[MAX_COOKIES];

static void cookie_free(struct cookie *c)
{
    free(c->name);
    free(c->value);
    free(c->domain);
    free(c->path);
    memset(c, 0, sizeof(*c));
}

static char *strndup_trim(const char *s, size_t n)
{
    while (n && (*s == ' ' || *s == '\t')) {
        s++;
        n--;
    }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        n--;
    return strndup(s, n);
}

/* The common HTTP-date form emitted by Date.toUTCString and Set-Cookie. */
static long long cookie_date(const char *s)
{
    const char *comma = strchr(s, ',');
    if (!comma)
        return 0;
    s = comma + 1;
    while (*s == ' ') s++;
    char *end;
    int day = (int)strtol(s, &end, 10);
    s = end; while (*s == ' ') s++;
    if (strlen(s) < 4)
        return 0;
    char month[4] = {s[0], s[1], s[2], 0};
    s += 3;
    int year = (int)strtol(s, &end, 10);
    s = end;
    int hour = (int)strtol(s, &end, 10);
    if (*end != ':') return 0;
    int minute = (int)strtol(end + 1, &end, 10);
    if (*end != ':') return 0;
    int second = (int)strtol(end + 1, &end, 10);
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int m;
    for (m = 0; m < 12 && strncasecmp(month, months + m * 3, 3); m++) {}
    if (m == 12 || year < 1601 || year > 9999 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
        return 0;
    long long days = 0;
    for (int y = 1970; y < year; y++)
        days += 365 + (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    for (int y = year; y < 1970; y++)
        days -= 365 + (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    for (int i = 0; i < m; i++)
        days += mdays[i] + (i == 1 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    long long stamp = ((days + day - 1) * 24 + hour) * 3600 + minute * 60 + second;
    return stamp ? stamp : 1; /* zero is reserved for session/invalid dates */
}
static void set_cookie(const struct url *u, const char *v, bool script)
{
    if (!v || strlen(v) > 4096 || v[strcspn(v, "\r\n")])
        return;
    struct cookie c;
    const char *semi = strchr(v, ';'), *eq = strchr(v, '=');
    size_t pair = semi ? (size_t)(semi - v) : strlen(v);
    bool remove = false, has_max_age = false;

    memset(&c, 0, sizeof(c));
    if (!eq || (size_t)(eq - v) >= pair)
        return;
    c.name = strndup_trim(v, (size_t)(eq - v));
    c.value = strndup_trim(eq + 1, pair - (size_t)(eq - v) - 1);
    c.host_only = true;
    for (const char *a = semi; a && *a;) {
        a++;
        const char *end = strchr(a, ';');
        size_t n = end ? (size_t)(end - a) : strlen(a);
        while (n && *a == ' ') {
            a++;
            n--;
        }
        const char *av = memchr(a, '=', n);
        size_t kn = av ? (size_t)(av - a) : n, vn = av ? n - kn - 1 : 0;
        if (kn == 6 && !strncasecmp(a, "domain", 6) && vn) {
            free(c.domain);
            c.domain = strndup_trim(av + 1 + (av[1] == '.'), vn - (av[1] == '.'));
            c.host_only = false;
        } else if (kn == 4 && !strncasecmp(a, "path", 4) && vn) {
            free(c.path);
            c.path = strndup_trim(av + 1, vn);
        } else if (kn == 7 && !strncasecmp(a, "max-age", 7) && vn) {
            char *age = strndup_trim(av + 1, vn), *end;
            if (age) {
                long seconds = strtol(age, &end, 10);
                if (end != age && !*end && (isdigit((unsigned char)*age) || *age == '-')) {
                    remove = seconds <= 0;
                    c.expires = plat_time() + seconds;
                    has_max_age = true;
                }
                free(age);
            }
        } else if (kn == 7 && !strncasecmp(a, "expires", 7) && vn && !has_max_age) {
            c.expires = cookie_date(av + 1);
        } else if (kn == 6 && !strncasecmp(a, "secure", 6)) {
            c.secure = true;
        } else if (kn == 8 && !strncasecmp(a, "httponly", 8)) {
            c.http_only = true;
        }
        a = end;
    }
    if (!c.domain)
        c.domain = strdup(u->host);
    if (!c.path || *c.path != '/') {
        free(c.path);
        /* Default path: the request path up to its last slash. */
        const char *slash = strrchr(u->path, '/');
        c.path = slash && slash != u->path ? strndup(u->path, (size_t)(slash - u->path)) : strdup("/");
    }
    if (!c.name || !*c.name || c.name[strcspn(c.name, " \t,;=")] || !c.value || !c.domain || !c.path) {
        cookie_free(&c);
        return;
    }
    /* A cookie's domain must cover the host that set it, and only https
     * may set a secure one. */
    size_t hl = strlen(u->host), dl = strlen(c.domain);
    if (dl > hl || strcasecmp(u->host + hl - dl, c.domain) || (dl < hl && u->host[hl - dl - 1] != '.') ||
        (c.secure && strcmp(u->scheme, "https")) || (script && c.http_only) ||
        (script && strcasecmp(c.domain, u->host)) ||
        (!strncmp(c.name, "__Secure-", 9) && !c.secure) ||
        (!strncmp(c.name, "__Host-", 7) && (!c.secure || !c.host_only || strcmp(c.path, "/")))) {
        cookie_free(&c);
        return;
    }
    int slot = -1, empty = -1;
    for (int i = 0; i < MAX_COOKIES; i++) {
        if (!jar[i].name) {
            if (empty < 0)
                empty = i;
        } else if (!strcmp(jar[i].name, c.name) && !strcasecmp(jar[i].domain, c.domain) &&
                   !strcmp(jar[i].path, c.path)) {
            slot = i;
        }
    }
    if (slot >= 0 && script && (jar[slot].http_only ||
        (jar[slot].secure && strcmp(u->scheme, "https")))) {
        cookie_free(&c);
        return;
    }
    if (c.expires && c.expires <= plat_time())
        remove = true;
    if (slot >= 0)
        cookie_free(&jar[slot]);
    else
        slot = empty >= 0 ? empty : (int)(plat_ms() % MAX_COOKIES);   /* full: evict one */
    if (jar[slot].name)
        cookie_free(&jar[slot]);
    if (remove)
        cookie_free(&c);
    else
        jar[slot] = c;
}

static char *cookies_for(const struct url *u, bool script)
{
    char *out = NULL;
    size_t len = 0;
    size_t hl = strlen(u->host);

    for (int i = 0; i < MAX_COOKIES; i++) {
        const struct cookie *c = &jar[i];
        if (!c->name || (script && c->http_only) ||
            (c->expires && c->expires <= plat_time()) || (c->secure && strcmp(u->scheme, "https")))
            continue;
        size_t dl = strlen(c->domain);
        bool dom = c->host_only ? !strcasecmp(u->host, c->domain)
                                : dl <= hl && !strcasecmp(u->host + hl - dl, c->domain) &&
                                      (dl == hl || u->host[hl - dl - 1] == '.');
        size_t pl = strlen(c->path);
        const char *path = u->path[0] ? u->path : "/";
        bool pathok = !strncmp(path, c->path, pl) && (path[pl] == 0 || path[pl] == '/' || c->path[pl - 1] == '/');
        if (!dom || !pathok)
            continue;
        size_t add = strlen(c->name) + strlen(c->value) + 3;
        char *n = realloc(out, len + add + 1);
        if (!n)
            break;
        out = n;
        len += (size_t)snprintf(out + len, add + 1, "%s%s=%s", len ? "; " : "", c->name, c->value);
    }
    return out;
}
char *ld_document_cookie(const char *url)
{
    struct url u;
    if (!url_parse(url, &u) || (strcmp(u.scheme, "http") && strcmp(u.scheme, "https")))
        return strdup("");
    char *text = cookies_for(&u, true);
    return text ? text : strdup("");
}
void ld_set_document_cookie(const char *url, const char *value)
{
    struct url u;
    if (url_parse(url, &u) && (!strcmp(u.scheme, "http") || !strcmp(u.scheme, "https")))
        set_cookie(&u, value, true);
}

/* ---- cache ---------------------------------------------------------------------- */

struct centry {
    char *url, *ctype;
    uint8_t *data;
    int len;
    uint32_t used;
};

static struct centry cache[CACHE_ENTRIES];
static int cache_bytes;
static uint32_t cache_clock;

static void cache_drop(struct centry *e)
{
    cache_bytes -= e->len;
    free(e->url);
    free(e->ctype);
    free(e->data);
    memset(e, 0, sizeof(*e));
}

static struct centry *cache_find(const char *url)
{
    for (int i = 0; i < CACHE_ENTRIES; i++)
        if (cache[i].url && !strcmp(cache[i].url, url))
            return &cache[i];
    return NULL;
}

static void cache_put(const char *url, const uint8_t *data, int len, const char *ctype)
{
    struct centry *e;

    if (len > CACHE_ENTRY_MAX)
        return;
    if ((e = cache_find(url)))
        cache_drop(e);
    for (;;) {
        /* A free slot once there's room, else evict the least recently used. */
        struct centry *oldest = NULL, *free_slot = NULL;
        for (int i = 0; i < CACHE_ENTRIES; i++) {
            if (!cache[i].url) {
                if (!free_slot)
                    free_slot = &cache[i];
            } else if (!oldest || cache[i].used < oldest->used) {
                oldest = &cache[i];
            }
        }
        if (free_slot && cache_bytes + len <= CACHE_BYTES) {
            e = free_slot;
            break;
        }
        if (!oldest)
            return;
        cache_drop(oldest);
    }
    e->url = strdup(url);
    e->data = malloc((size_t)len + 1);
    e->ctype = ctype ? strdup(ctype) : NULL;
    if (!e->url || !e->data) {
        cache_drop(e);
        return;
    }
    memcpy(e->data, data, (size_t)len);
    e->len = len;
    e->used = ++cache_clock;
    cache_bytes += len;
}

/* ---- local schemes ---------------------------------------------------------------- */

static const char *type_of(const char *path)
{
    const char *dot = strrchr(path, '.');

    if (!dot)
        return "text/plain";
    if (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm"))
        return "text/html";
    if (!strcasecmp(dot, ".js"))
        return "application/javascript";
    if (!strcasecmp(dot, ".css"))
        return "text/css";
    if (!strcasecmp(dot, ".png"))
        return "image/png";
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg"))
        return "image/jpeg";
    if (!strcasecmp(dot, ".gif"))
        return "image/gif";
    return "text/plain";
}

static int unhex(int c)
{
    return isdigit(c) ? c - '0' : isxdigit(c) ? (tolower(c) - 'a' + 10) : -1;
}

/* Percent-decodes s into out (which may be s); returns the length. */
static int percent_decode(const char *s, int n, char *out)
{
    int o = 0;

    for (int i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n && unhex((unsigned char)s[i + 1]) >= 0 && unhex((unsigned char)s[i + 2]) >= 0) {
            out[o++] = (char)(unhex((unsigned char)s[i + 1]) << 4 | unhex((unsigned char)s[i + 2]));
            i += 2;
        } else {
            out[o++] = s[i];
        }
    }
    return o;
}

static int b64(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+' || c == '-')
        return 62;
    if (c == '/' || c == '_')
        return 63;
    return -1;
}

static int load_data(struct fetch *f)
{
    const char *s = f->url + 5, *comma = strchr(s, ',');

    if (!comma)
        return WE_BADURL;
    bool base64 = comma - s >= 7 && !strncasecmp(comma - 7, ";base64", 7);
    int mt = (int)strcspn(s, ";,");
    f->r.ctype = mt ? strndup(s, (size_t)mt) : strdup("text/plain");
    const char *d = comma + 1;
    int n = (int)strlen(d);
    uint8_t *out = malloc((size_t)n + 1);
    if (!out)
        return WE_NOMEM;
    int o = percent_decode(d, n, (char *)out);
    if (base64) {
        int bits = 0, acc = 0, k = 0;
        for (int i = 0; i < o; i++) {
            int v = b64(out[i]);
            if (v < 0)
                continue;
            acc = acc << 6 | v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out[k++] = (uint8_t)(acc >> bits);
            }
        }
        o = k;
    }
    f->r.data = out;
    f->r.len = o;
    return 0;
}

static int load_local(struct fetch *f, const struct url *u)
{
    char path[URL_PART_MAX];
    int n = percent_decode(u->path, (int)strlen(u->path), path);

    path[n] = 0;
    if (!strcmp(u->scheme, "res")) {
        uint32_t size;
        const char *name = path[0] == '/' ? path + 1 : path;
        const void *d = plat_res(name, &size);
        if (!d)
            return WE_NOTFOUND;
        f->r.data = malloc(size + 1);
        if (!f->r.data)
            return WE_NOMEM;
        memcpy(f->r.data, d, size);
        f->r.len = (int)size;
    } else {
        int rv = plat_read_file(path, &f->r.data, &f->r.len);
        if (rv < 0)
            return rv;
    }
    f->r.ctype = strdup(type_of(path));
    return 0;
}

/* ---- fetches ------------------------------------------------------------------------ */

static void finish(struct fetch *f, int err)
{
    f->r.err = err;
    if (!err && !f->r.status)
        f->r.status = 200;
    if (!f->r.url)
        f->r.url = strdup(f->url);
    http_free(f->h);
    f->h = NULL;
    f->active = false;
    f->done = true;
}

/* Starts (or restarts, after a redirect) the request for f->url. */
static void begin(struct fetch *f)
{
    static struct url u;

    if (!strncasecmp(f->url, "data:", 5)) {
        finish(f, load_data(f));
        return;
    }
    if (!url_parse(f->url, &u)) {
        finish(f, WE_BADURL);
        return;
    }
    if (!strcmp(u.scheme, "res") || !strcmp(u.scheme, "file")) {
        finish(f, load_local(f, &u));
        return;
    }
    if (strcmp(u.scheme, "http") && strcmp(u.scheme, "https")) {
        finish(f, WE_SCHEME);
        return;
    }
    if (!f->post && !f->reload && f->kind!=F_DOC && f->kind!=F_FRAME) {
        struct centry *e = cache_find(f->url);
        if (e) {
            e->used = ++cache_clock;
            f->r.data = malloc((size_t)e->len + 1);
            if (f->r.data) {
                memcpy(f->r.data, e->data, (size_t)e->len);
                f->r.len = e->len;
                f->r.ctype = e->ctype ? strdup(e->ctype) : NULL;
                finish(f, 0);
                return;
            }
        }
    }
    char *cookies = cookies_for(&u, false);
    f->h = http_start(f->url, f->post, f->post_len, f->post_type, cookies, max_body[f->kind]);
    free(cookies);
    if (!f->h)
        finish(f, WE_BADURL);
}

/* A response is in: redirect, or complete. */
static void response(struct fetch *f)
{
    static struct url u;
    int status = http_status(f->h);
    const char *v;

    if (url_parse(f->url, &u))
        for (int i = 0; (v = http_header(f->h, "set-cookie", i)); i++)
            set_cookie(&u, v, false);
    const char *loc = http_header(f->h, "location", 0);
    if (status >= 300 && status < 400 && status != 304 && loc) {
        char *next = url_resolve(f->url, loc);
        if (!next) {
            finish(f, WE_BADURL);
            return;
        }
        if(strncasecmp(next,"http://",7) && strncasecmp(next,"https://",8)) {
            free(next);finish(f,WE_SCHEME);return;
        }
        if (++f->redirects > MAX_REDIRECTS) {
            free(next);
            finish(f, WE_REDIRECTS);
            return;
        }
        /* 303, and in practice 301/302, turn a POST into a GET. */
        if (status != 307 && status != 308) {
            free(f->post);
            f->post = NULL;
        }
        free(f->url);
        f->url = next;
        http_free(f->h);
        f->h = NULL;
        begin(f);
        return;
    }
    f->r.status = status;
    f->r.data = http_take_body(f->h, &f->r.len);
    v = http_header(f->h, "content-type", 0);
    f->r.ctype = v ? strdup(v) : NULL;
    v=http_header(f->h,"x-frame-options",0);f->r.xfo=v ? strdup(v) : NULL;
    if(http_header(f->h,"x-frame-options",1)) { free(f->r.xfo);f->r.xfo=strdup("DENY"); }
    v=http_header(f->h,"content-security-policy",0);f->r.csp=v ? strdup(v) : NULL;
    if(http_header(f->h,"content-security-policy",1)) {
        free(f->r.csp);f->r.csp=strdup("script-src 'none'; frame-ancestors 'none'");
    }
    v = http_header(f->h, "cache-control", 0);
    if (status == 200 && !f->post && f->r.data && f->kind!=F_DOC && f->kind!=F_FRAME && !(v && strstr(v, "no-store")))
        cache_put(f->url, f->r.data, f->r.len, f->r.ctype);
    finish(f, f->r.data ? 0 : WE_NOMEM);
}

bool ld_can_start(void) {
    for(int i=0;i<MAX_FETCHES;i++)if(!fetches[i].used)return true;
    return false;
}

int ld_start(const char *url, int kind, int tag, const char *post, const char *post_type, bool reload)
{
    struct fetch *f = NULL;

    if (kind < F_DOC || kind > F_IMG) return WE_BADURL;
    for (int i = 0; i < MAX_FETCHES && !f; i++)
        if (!fetches[i].used)
            f = &fetches[i];
    if (!f)
        return WE_NOMEM;
    memset(f, 0, sizeof(*f));
    f->used = true;
    f->id = next_id++;
    f->kind = (uint8_t)kind;
    f->tag = tag;
    f->reload = reload;
    f->url = strdup(url);
    if (post) {
        f->post_len = (int)strlen(post);
        f->post = strdup(post);
        f->post_type = post_type ? strdup(post_type) : NULL;
    }
    f->r.id = f->id;
    f->r.kind = kind;
    f->r.tag = tag;
    if (!f->url || (post && !f->post))
        finish(f, WE_NOMEM);
    return f->id;
}

static struct fetch *by_id(int id)
{
    for (int i = 0; i < MAX_FETCHES; i++)
        if (fetches[i].used && fetches[i].id == id)
            return &fetches[i];
    return NULL;
}

static void release(struct fetch *f)
{
    http_free(f->h);
    free(f->url);
    free(f->post);
    free(f->post_type);
    if (f->done)
        ld_free_result(&f->r);
    memset(f, 0, sizeof(*f));
}

void ld_cancel(int id)
{
    struct fetch *f = by_id(id);

    if (f)
        release(f);
}

void ld_cancel_all(void)
{
    for (int i = 0; i < MAX_FETCHES; i++)
        if (fetches[i].used)
            release(&fetches[i]);
}

bool ld_busy(void)
{
    for (int i = 0; i < MAX_FETCHES; i++)
        if (fetches[i].used && !fetches[i].done)
            return true;
    return false;
}

/* The next queued fetch to start: lowest kind, then oldest. */
static struct fetch *next_queued(void)
{
    struct fetch *best = NULL;

    for (int i = 0; i < MAX_FETCHES; i++) {
        struct fetch *f = &fetches[i];
        if (!f->used || f->active || f->done)
            continue;
        if (!best || f->kind < best->kind || (f->kind == best->kind && f->id < best->id))
            best = f;
    }
    return best;
}

void ld_step(int budget_ms)
{
    uint32_t start = plat_ms();

    do {
        int active = 0;
        for (int i = 0; i < MAX_FETCHES; i++)
            active += fetches[i].active;
        struct fetch *f;
        while (active < MAX_ACTIVE && (f = next_queued())) {
            f->active = true;
            begin(f);
            active += f->active;
        }
        bool progress = false;
        for (int i = 0; i < MAX_FETCHES; i++) {
            f = &fetches[i];
            if (!f->active || !f->h)
                continue;
            int rv = http_step(f->h);
            if (rv < 0)
                finish(f, rv);
            else if (rv > 0)
                response(f);
            progress |= rv != 0;
        }
        if (!progress)
            break;
    } while ((int)(plat_ms() - start) < budget_ms);
}

bool ld_take(struct fetch_result *r)
{
    struct fetch *best = NULL;

    for (int i = 0; i < MAX_FETCHES; i++)
        if (fetches[i].used && fetches[i].done && (!best || fetches[i].id < best->id))
            best = &fetches[i];
    if (!best)
        return false;
    *r = best->r;
    best->done = false;         /* the result now belongs to the caller */
    release(best);
    return true;
}

void ld_progress(int id, int *got, int *total)
{
    struct fetch *f = by_id(id);

    *got = *total = 0;
    if (f && f->h)
        http_progress(f->h, got, total);
}

void ld_free_result(struct fetch_result *r)
{
    free(r->data);
    free(r->ctype);
    free(r->url);
    free(r->xfo);
    free(r->csp);
    r->xfo = r->csp = NULL;
    r->data = NULL;
    r->ctype = NULL;
    r->url = NULL;
}
