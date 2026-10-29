#include "codex.h"
#include "net.h"
#include "web.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
/* Negative sink return ends the HTTP transfer after an authenticated SSE
 * terminal event; transport framing need not wait for a server keepalive. */
#define CX_FINISHED (-2001)
struct transfer { struct http *http; int (*sink)(const uint8_t *, int, void *); void *arg; };
static int receive(const uint8_t *data, int n, void *arg)
{
    struct transfer *t = arg;
    if (http_status(t->http) != 200) return 0;
    const char *type = http_header(t->http, "content-type", 0);
    if (!type || strncasecmp(type, "text/event-stream", 17) || (type[17] && type[17] != ';' && type[17] != ' ')) return WE_PROTOCOL;
    return t->sink(data, n, t->arg);
}
int cx_http_post(struct cx_io *io, const char *url, const char *type, const char *body,
                 const char *bearer, const char *account, int (*sink)(const uint8_t *, int, void *),
                 void *arg, struct cx_response *out)
{
    memset(out, 0, sizeof(*out));
    if (!io->active(io->arg)) return -2;
#ifdef CHATGPT_TEST_ORIGIN
    char mapped[512];
    const char *path = strchr(url + 8, '/');
    if (!path) return WE_BADURL;
    snprintf(mapped, sizeof(mapped), "%s%s", CHATGPT_TEST_ORIGIN, path);
    url = mapped;
#endif
    struct transfer t = {.sink = sink, .arg = arg};
    struct http_post_options options = {.content_type = type, .bearer = bearer, .account = account,
        .max_body = sink ? CX_STREAM_MAX : CX_AUTH_MAX, .idle_ms = sink ? 90000 : 30000,
        .sink = sink ? receive : NULL, .sink_arg = &t};
    t.http = http_start_post(url, body, (int)strlen(body), &options);
    if (!t.http) return WE_NOMEM;
    uint32_t start = io->millis(io->arg); int rv = 0;
    while (io->active(io->arg)) {
        rv = http_step(t.http);
        if (rv == CX_FINISHED) rv = 1;
        if (rv) break;
        if (io->millis(io->arg) - start > (sink ? 180000u : 60000u)) { rv = WE_TIMEOUT; break; }
        if (!io->sleep(io->arg, 10)) break;
    }
    if (!io->active(io->arg)) rv = -2;
    if (rv == 1) {
        out->status = http_status(t.http);
        const char *content = http_header(t.http, "content-type", 0);
        snprintf(out->type, sizeof(out->type), "%s", content ? content : "");
        if (!sink) { int n; out->body = (char *)http_take_body(t.http, &n); out->len = (size_t)n; }
    }
    http_free(t.http);
    return rv == 1 ? 0 : rv ? rv : -2;
}
