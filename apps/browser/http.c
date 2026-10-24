/*
 * HTTP/1.1 client: one request per connection (Connection: close), over
 * TLS for https, as a state machine stepped by the loader. Handles
 * Content-Length, chunked transfer coding, gzip/deflate content coding, and
 * interim 1xx replies.
 * Redirects and cookies are the loader's business.
 */

#include "net.h"
#include "plat.h"
#include "url.h"
#include "web.h"
#include "identity.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define HEAD_MAX        (16 * 1024)
#define CONNECT_MS      30000   /* including the TLS handshake */
#define IDLE_MS         30000

enum { H_RESOLVE, H_CONNECT, H_SEND, H_HEAD, H_BODY, H_DONE };
enum { CH_SIZE, CH_DATA, CH_CRLF, CH_TRAILER };

struct http {
    int state;
    char host[URL_HOST_MAX];
    int port;
    bool https;
    int resolve_id;
    uint32_t cached_addr;
    bool have_addr;
    struct conn c;
    char *req;
    int req_len, req_off;
    char *head;                 /* response header block, NUL-separated lines */
    int head_len, nlines;
    int status;
    int content_length;         /* -1 unknown */
    bool chunked;
    int chunk_state, chunk_left;
    char chunk_line[24];
    int chunk_line_len;
    uint8_t *body;
    int body_len, body_cap, max_body;
    int (*sink)(const uint8_t *, int, void *);
    void *sink_arg;
    uint32_t last;              /* ms of the last progress */
};

/* Recent lookups: every image on a page would otherwise ask again (on the
 * device a lookup holds up the UI thread). */
#define DNS_CACHE   16
#define DNS_TTL_MS  (10 * 60 * 1000)

static struct {
    char host[URL_HOST_MAX];
    uint32_t addr, when;
} dns_cache[DNS_CACHE];

static bool dns_cached(const char *host, uint32_t *addr)
{
    for (int i = 0; i < DNS_CACHE; i++) {
        if (dns_cache[i].host[0] && !strcmp(dns_cache[i].host, host) &&
            plat_ms() - dns_cache[i].when < DNS_TTL_MS) {
            *addr = dns_cache[i].addr;
            return true;
        }
    }
    return false;
}

static void dns_remember(const char *host, uint32_t addr)
{
    int slot = 0;

    for (int i = 1; i < DNS_CACHE; i++)
        if (!strcmp(dns_cache[i].host, host) || dns_cache[i].when < dns_cache[slot].when)
            slot = i;
    strcpy(dns_cache[slot].host, host);
    dns_cache[slot].addr = addr;
    dns_cache[slot].when = plat_ms();
}

static int buf_append(struct http *h, const uint8_t *d, int n)
{
    if (n < 0 || n > h->max_body - h->body_len)
        return WE_TOO_BIG;
    if (h->sink) {
        int rv = h->sink(d, n, h->sink_arg);
        if (rv) return rv;
        h->body_len += n;
        return 0;
    }
    if (h->body_len + n > h->body_cap) {
        int cap = h->body_cap ? h->body_cap : 16 * 1024;
        while (cap < h->body_len + n)
            cap *= 2;
        if (cap > h->max_body)
            cap = h->max_body;
        uint8_t *b = realloc(h->body, (size_t)cap);
        if (!b)
            return WE_NOMEM;
        h->body = b;
        h->body_cap = cap;
    }
    memcpy(h->body + h->body_len, d, (size_t)n);
    h->body_len += n;
    return 0;
}

struct http *http_start(const char *url, const char *post, int post_len, const char *post_type,
                        const char *cookies, int max_body)
{
    static struct url u;
    struct http *h;

    if (!url_parse(url, &u) || !u.has_auth || !u.host[0])
        return NULL;
    h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->c.fd = -1;
    h->resolve_id = -1;
    h->content_length = -1;
    h->max_body = max_body;
    strcpy(h->host, u.host);
    h->https = !strcmp(u.scheme, "https");
    h->port = u.port > 0 ? u.port : url_default_port(u.scheme);

    size_t cap = strlen(u.path) + strlen(u.query) + strlen(u.host) + (cookies ? strlen(cookies) : 0) +
                 (size_t)(post ? post_len : 0) + 512;
    h->req = malloc(cap);
    if (!h->req) {
        free(h);
        return NULL;
    }
    char hostport[URL_HOST_MAX + 8];
    if (h->port != url_default_port(u.scheme))
        snprintf(hostport, sizeof(hostport), "%s:%d", u.host, h->port);
    else
        snprintf(hostport, sizeof(hostport), "%s", u.host);
    int n = snprintf(h->req, cap,
                     "%s %s%s%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " BROWSER_USER_AGENT "\r\n"
                     "Accept: text/html,application/xhtml+xml,image/png,image/jpeg,image/gif,*/*;q=0.5\r\n"
                     "Accept-Encoding: gzip, deflate\r\nAccept-Language: " BROWSER_LANGUAGE "\r\nConnection: close\r\n",
                     post ? "POST" : "GET", u.path[0] ? u.path : "/", u.has_query ? "?" : "", u.query, hostport);
    if (cookies && *cookies)
        n += snprintf(h->req + n, cap - (size_t)n, "Cookie: %s\r\n", cookies);
    if (post)
        n += snprintf(h->req + n, cap - (size_t)n, "Content-Type: %s\r\nContent-Length: %d\r\n",
                      post_type ? post_type : "application/x-www-form-urlencoded", post_len);
    n += snprintf(h->req + n, cap - (size_t)n, "\r\n");
    if (post) {
        memcpy(h->req + n, post, (size_t)post_len);
        n += post_len;
    }
    h->req_len = n;
    h->last = plat_ms();

    uint32_t addr;
    if (dns_cached(h->host, &addr)) {
        h->resolve_id = -1;
        h->cached_addr = addr;
        h->have_addr = true;
    } else {
        h->resolve_id = plat_resolve_start(h->host);    /* an error is reported by the first step */
    }
    return h;
}

struct http *http_start_stream(const char *url, int max_body,
                               int (*sink)(const uint8_t *, int, void *), void *arg)
{
    if (!sink) return NULL;
    struct http *h = http_start(url, NULL, 0, NULL, NULL, max_body);
    if (!h) return NULL;
    /* Same-length substitution keeps request offsets unchanged. */
    char *encoding = strstr(h->req, "gzip, deflate");
    if (encoding) memcpy(encoding, "identity     ", 13);
    h->sink = sink;
    h->sink_arg = arg;
    return h;
}

/* Header block parsed: status line and fields. */
static int parse_head(struct http *h)
{
    /* Split into NUL-terminated lines, dropping CRs. */
    int o = 0;
    h->nlines = 0;
    for (int i = 0; i < h->head_len; i++) {
        char c = h->head[i];
        if (c == '\r')
            continue;
        if (c == '\n') {
            h->head[o++] = 0;
            h->nlines++;
        } else {
            h->head[o++] = c;
        }
    }
    h->head_len = o;
    if (strncmp(h->head, "HTTP/", 5))
        return WE_PROTOCOL;
    const char *sp = strchr(h->head, ' ');
    if (!sp || !isdigit((unsigned char)sp[1]))
        return WE_PROTOCOL;
    h->status = atoi(sp + 1);
    const char *v = http_header(h, "content-length", 0);
    h->content_length = -1;
    if (v) {
        if (!*v) return WE_PROTOCOL;
        unsigned len = 0;
        for (const char *p = v; *p; p++) {
            if (*p < '0' || *p > '9' || len > (0x7fffffffu - (*p - '0')) / 10)
                return WE_PROTOCOL;
            len = len * 10 + (*p - '0');
        }
        h->content_length = (int)len;
    }
    if (h->sink) {
        v = http_header(h, "content-encoding", 0);
        if (v && strcasecmp(v, "identity")) return WE_DECODE;
    }
    v = http_header(h, "transfer-encoding", 0);
    h->chunked = v && strstr(v, "chunked");
    if (h->content_length > h->max_body)
        return WE_TOO_BIG;
    return 0;
}

const char *http_header(const struct http *h, const char *name, int n)
{
    size_t nl = strlen(name);
    const char *line = h->head;

    if (!h->head)
        return NULL;
    for (int i = 0; i < h->nlines; i++, line += strlen(line) + 1) {
        if (i == 0 || strlen(line) <= nl || strncasecmp(line, name, nl) || line[nl] != ':')
            continue;
        if (n-- > 0)
            continue;
        const char *v = line + nl + 1;
        while (*v == ' ' || *v == '\t')
            v++;
        return v;
    }
    return NULL;
}

int http_status(const struct http *h)
{
    return h->status;
}

/* Body bytes, through the chunked decoder if needed. */
static int body_bytes(struct http *h, const uint8_t *d, int n)
{
    if (!h->chunked) {
        if (h->content_length >= 0 && n > h->content_length - h->body_len)
            return WE_PROTOCOL;
        int rv = buf_append(h, d, n);
        if (rv)
            return rv;
        if (h->content_length >= 0 && h->body_len >= h->content_length)
            h->state = H_DONE;
        return 0;
    }
    for (int i = 0; i < n && h->state != H_DONE;) {
        switch (h->chunk_state) {
        case CH_SIZE:
        case CH_TRAILER:
            if (d[i] == '\n') {
                h->chunk_line[h->chunk_line_len] = 0;
                if (h->chunk_state == CH_TRAILER) {
                    if (h->chunk_line_len == 0)
                        h->state = H_DONE;      /* blank line after trailers */
                } else {
                    char *end;
                    unsigned long chunk = strtoul(h->chunk_line, &end, 16);
                    if (end == h->chunk_line || (*end && *end != ';') || chunk > 0x7fffffffUL)
                        return WE_PROTOCOL;
                    h->chunk_left = (int)chunk;
                    h->chunk_state = h->chunk_left ? CH_DATA : CH_TRAILER;
                }
                h->chunk_line_len = 0;
            } else if (d[i] != '\r') {
                if (h->chunk_line_len >= (int)sizeof(h->chunk_line) - 1) return WE_PROTOCOL;
                h->chunk_line[h->chunk_line_len++] = (char)d[i];
            }
            i++;
            break;
        case CH_DATA: {
            int k = n - i < h->chunk_left ? n - i : h->chunk_left;
            int rv = buf_append(h, d + i, k);
            if (rv)
                return rv;
            i += k;
            h->chunk_left -= k;
            if (!h->chunk_left)
                h->chunk_state = CH_CRLF;
            break;
        }
        case CH_CRLF:
            if (d[i] == '\n')
                h->chunk_state = CH_SIZE;
            i++;
            break;
        }
    }
    return 0;
}

/* Content coding, once the body is complete. */
static int decode_body(struct http *h)
{
    if (h->sink) return 0;
    const char *ce = http_header(h, "content-encoding", 0);

    if (!ce || !h->body_len || (!strstr(ce, "gzip") && !strstr(ce, "deflate")))
        return 0;
    const uint8_t *d = h->body;
    int n = h->body_len, out_len = 0;
    bool zlib = true;
    if (strstr(ce, "gzip")) {
        /* RFC 1952 header, then raw deflate. */
        if (n < 18 || d[0] != 0x1F || d[1] != 0x8B || d[2] != 8)
            return WE_DECODE;
        int flags = d[3], off = 10;
        if (flags & 4)
            off += 2 + (d[off] | d[off + 1] << 8);
        if (flags & 8)
            while (off < n && d[off++])
                ;
        if (flags & 16)
            while (off < n && d[off++])
                ;
        if (flags & 2)
            off += 2;
        if (off >= n)
            return WE_DECODE;
        d += off;
        n -= off;
        zlib = false;
    } else if (n < 2 || (d[0] & 0x0F) != 8 || ((d[0] << 8) | d[1]) % 31) {
        zlib = false;           /* "deflate" sent as raw deflate */
    }
    uint8_t *out = web_inflate(d, n, zlib, &out_len);
    if (!out)
        return WE_DECODE;
    if (out_len > h->max_body) {
        free(out);
        return WE_TOO_BIG;
    }
    free(h->body);
    h->body = out;
    h->body_len = h->body_cap = out_len;
    return 0;
}

int http_step(struct http *h)
{
    uint8_t buf[2048];
    uint32_t now = plat_ms();
    int rv;

    for (int rounds = 0; rounds < 16; rounds++) {
        switch (h->state) {
        case H_RESOLVE: {
            uint32_t addr = h->cached_addr;
            if (!h->have_addr) {
                if (h->resolve_id < 0)
                    return h->resolve_id;
                rv = plat_resolve_poll(h->resolve_id, &addr);
                if (rv == 0)
                    return 0;
                h->resolve_id = -1;
                if (rv < 0)
                    return rv;
                dns_remember(h->host, addr);
            }
            if ((rv = conn_open(&h->c, addr, h->port, h->https ? h->host : NULL)) < 0)
                return rv;
            h->state = H_CONNECT;
            h->last = now;
            break;
        }
        case H_CONNECT:
            rv = conn_ready(&h->c);
            if (rv < 0)
                return rv;
            if (rv == 0)
                return (int)(now - h->last) > CONNECT_MS ? WE_TIMEOUT : 0;
            h->state = H_SEND;
            h->last = now;
            break;
        case H_SEND:
            rv = conn_write(&h->c, h->req + h->req_off, h->req_len - h->req_off);
            if (rv == -CONN_AGAIN)
                return (int)(now - h->last) > IDLE_MS ? WE_TIMEOUT : 0;
            if (rv < 0)
                return rv;
            h->req_off += rv;
            h->last = now;
            if (h->req_off == h->req_len) {
                h->state = H_HEAD;
                h->head = malloc(HEAD_MAX + 1);
                if (!h->head)
                    return WE_NOMEM;
            }
            break;
        case H_HEAD:
        case H_BODY: {
            rv = conn_read(&h->c, buf, sizeof(buf));
            if (rv == -CONN_AGAIN)
                return (int)(now - h->last) > IDLE_MS ? WE_TIMEOUT : 0;
            if (rv < 0)
                return rv;
            h->last = now;
            if (rv == 0) {
                /* End of stream: the body is complete unless a length or
                 * the chunked coding says otherwise. */
                if (h->state == H_HEAD || h->chunked ||
                    (h->content_length >= 0 && h->body_len < h->content_length))
                    return WE_RESET;
                h->state = H_DONE;
                break;
            }
            int i = 0;
            if (h->state == H_HEAD) {
                /* Byte by byte until the blank line ends the header. */
                for (; i < rv; i++) {
                    if (h->head_len >= HEAD_MAX)
                        return WE_PROTOCOL;
                    h->head[h->head_len++] = (char)buf[i];
                    int L = h->head_len;
                    if ((L >= 4 && !memcmp(h->head + L - 4, "\r\n\r\n", 4)) ||
                        (L >= 2 && !memcmp(h->head + L - 2, "\n\n", 2))) {
                        i++;
                        int e = parse_head(h);
                        if (e < 0)
                            return e;
                        if (h->status >= 100 && h->status < 200) {
                            h->head_len = 0;    /* interim reply: next header */
                            continue;
                        }
                        h->state = H_BODY;
                        if (h->status == 204 || h->status == 304 || h->content_length == 0)
                            h->state = H_DONE;
                        break;
                    }
                }
                if (h->state != H_BODY)
                    break;
            }
            if (h->state == H_BODY && i < rv && (rv = body_bytes(h, buf + i, rv - i)) < 0)
                return rv;
            break;
        }
        case H_DONE:
            conn_close(&h->c);
            if ((rv = decode_body(h)) < 0)
                return rv;
            return 1;
        }
    }
    return 0;
}

uint8_t *http_take_body(struct http *h, int *len)
{
    uint8_t *b = h->body;

    *len = h->body_len;
    h->body = NULL;
    h->body_len = h->body_cap = 0;
    if (!b)
        b = malloc(1);
    return b;
}

void http_progress(const struct http *h, int *got, int *total)
{
    *got = h->body_len;
    *total = h->content_length > 0 ? h->content_length : 0;
}

void http_free(struct http *h)
{
    if (!h)
        return;
    if (h->resolve_id >= 0)
        plat_resolve_cancel(h->resolve_id);
    conn_close(&h->c);
    free(h->req);
    free(h->head);
    free(h->body);
    free(h);
}
