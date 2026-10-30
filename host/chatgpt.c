/* Native exercise of the production JSON/HTTP/TLS path, without a GUI. */
#include "../packages/chatgpt/chat.h"
#include "../packages/chatgpt/codex.h"
#include "../apps/browser/net.h"
#include "../apps/browser/plat.h"
#include "../apps/browser/web.h"
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef __APPLE__
#include <sys/random.h>
#endif
#include <time.h>
#include <unistd.h>
uint32_t plat_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
static uint32_t address;
static int resolved;
int plat_resolve_start(const char *host)
{
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    resolved = getaddrinfo(host, NULL, &hints, &res) ? WE_DNS : 1;
    if (res) { address = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr; freeaddrinfo(res); }
    return 0;
}
int plat_resolve_poll(int id, uint32_t *addr) { (void)id; *addr = address; return resolved; }
void plat_resolve_cancel(int id) { (void)id; }
static uint8_t *roots;
const void *plat_roots(uint32_t *size)
{
    const char *path = getenv("CHATGPT_ROOTS");
    FILE *file = fopen(path ? path : "../assets/ssl/roots.der", "rb");
    *size = 0;
    if (!file) return NULL;
    fseek(file, 0, SEEK_END); long len = ftell(file); rewind(file);
    if (len > 0 && len <= 1024 * 1024) {
        roots = malloc((size_t)len);
        if (roots && fread(roots, 1, (size_t)len, file) == (size_t)len) *size = (uint32_t)len;
    }
    fclose(file); return roots;
}
long long plat_time(void) { return time(NULL); }
void plat_noise(uint8_t *buf, int n)
{
#ifdef __APPLE__
    arc4random_buf(buf, (size_t)n);
#else
    int off = 0;
    while (off < n) { ssize_t rv = getrandom(buf + off, (size_t)(n - off), 0); if (rv <= 0) abort(); off += (int)rv; }
#endif
}
int plat_load_seed(uint8_t *seed) { plat_noise(seed, PLAT_SEED_LEN); return 0; }
void plat_save_seed(const uint8_t *seed) { (void)seed; }
struct context { const char *base; bool canceled; int saves; uint32_t started, cancel_ms; };
static bool active(void *arg)
{
    struct context *c = arg;
    return !c->canceled && (!c->cancel_ms || plat_ms() - c->started < c->cancel_ms);
}
static bool pause_ms(void *arg, uint32_t ms)
{
    uint32_t start = plat_ms();
    while (active(arg)) { if (plat_ms() - start >= ms) return true; usleep(1000); }
    return false;
}
static uint32_t millis(void *arg) { (void)arg; return plat_ms(); }
static uint32_t unix_time(void *arg) { (void)arg; return (uint32_t)time(NULL); }
static bool save(void *arg, const struct cx_tokens *t)
{
    struct context *c = arg; c->saves++;
    const char *file = getenv("CX_TEST_STORE");
    if (file) {
        FILE *f = fopen(file, "wb"); if (!f) return false;
        bool ok = fwrite(t, 1, sizeof(*t), f) == sizeof(*t); fclose(f); return ok;
    }
    return true;
}
static void progress(void *arg, const char *text, bool device)
{
    (void)arg; (void)text;
    puts(device ? "device-code-ready" : "reply-progress");
}
/* This mapper is only in the native test executable. The shipped app has
 * fixed HTTPS authorities and cannot direct credentials to a custom host. */
static int post(struct cx_io *io, const char *url, const char *type, const char *body,
                const char *bearer, const char *account, int (*sink)(const uint8_t *, int, void *),
                void *arg, struct cx_response *r)
{
    struct context *c = io->arg; char mapped[512];
    const char *path = strchr(url + 8, '/'); if (!path) return -1;
    snprintf(mapped, sizeof(mapped), "%s%s", c->base, path);
    return cx_http_post(io, mapped, type, body, bearer, account, sink, arg, r);
}
int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    struct context c = {.base = argv[1], .started = plat_ms()};
    const char *cancel = getenv("CX_TEST_CANCEL_MS"); if (cancel) c.cancel_ms = (uint32_t)strtoul(cancel, NULL, 10);
    struct cx_io io = {.arg = &c, .active = active, .sleep = pause_ms, .millis = millis, .time = unix_time,
                       .save = save, .progress = progress, .post = post};
    struct cx_tokens *tokens = calloc(1, sizeof(*tokens));
    char *out = calloc(1, CHAT_TEXT); if (!tokens || !out) return 2;
    int rv = -1;
    if (!strcmp(argv[2], "device")) rv = cx_device_login(&io, tokens, out, CHAT_TEXT);
    else if (argc >= 4) {
        FILE *file = fopen(argv[3], "rb"); char *data = malloc(CX_AUTH_MAX + 1);
        if (!file || !data) return 2;
        size_t n = fread(data, 1, CX_AUTH_MAX + 1, file); fclose(file);
        if (cx_import(data, n, tokens)) rv = 0;
        cx_wipe(data, CX_AUTH_MAX + 1); free(data);
    }
    if (!rv && strcmp(argv[2], "import")) {
        struct chat_history *h = calloc(1, sizeof(*h)); char *body = malloc(CHAT_WIRE);
        if (!h || !body) return 2;
        h->version = 1; chat_append(h, false, "Hello, Glaze!");
        if (chat_codex_request(h, "gpt-6-luna", body, CHAT_WIRE) < 0) rv = -1;
        else rv = cx_chat(&io, tokens, body, out, CHAT_TEXT);
        free(h); free(body);
    }
    printf("result=%s saved=%d signed_in=%d\n%s\n", rv ? "error" : "ok", c.saves, cx_tokens_valid(tokens), out);
    cx_wipe(tokens, sizeof(*tokens)); free(tokens); free(out); free(roots); return rv ? 1 : 0;
}
