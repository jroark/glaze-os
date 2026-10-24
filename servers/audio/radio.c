/* Radio network worker. Never touches decoder/output state. Only bounded
 * copies and status updates hold the lock; DNS and TLS run outside it. */
#include "radio.h"
#include "radio_protocol.h"
#include "net.h"
#include "url.h"
#include "web.h"
#include <glaze/net.h>
#include <glaze/sys.h>
#include <glaze/thread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define CANCELLED (-200)
#define PLAYLIST_MAX 32768
#define IDLE_MS 15000u
static struct gz_mutex lock = GZ_MUTEX_INIT;
static int wake;
static bool requested;
static uint32_t generation, epoch, used, read_pos, write_pos;
static uint8_t *ring;
static struct radio_request request;
static struct radio_state state;

static bool current(uint32_t gen)
{
    gz_mutex_lock(&lock);
    bool ok = requested && gen == generation;
    gz_mutex_unlock(&lock);
    return ok;
}
static bool wait_current(uint32_t gen, uint32_t ms)
{
    uint32_t start = gz_time_ms();
    while (current(gen)) {
        if (gz_time_ms() - start >= ms) return true;
        gz_sleep_ms(10);
    }
    return false;
}
static uint32_t public_error(int rv)
{
    switch (rv) {
    case WE_NETDOWN: return RADIO_E_NETWORK;
    case WE_TIMEOUT: return RADIO_E_TIMEOUT;
    case WE_CERT: case WE_CERT_NAME: case WE_TLS: return RADIO_E_CERT;
    case WE_CERT_DATE: return RADIO_E_DATE;
    case WE_NOMEM: return RADIO_E_MEMORY;
    case WE_DECODE: return RADIO_E_FORMAT;
    case WE_PROTOCOL: case WE_TOO_BIG: return RADIO_E_PLAYLIST;
    default: return RADIO_E_CONNECTION;
    }
}
struct transfer {
    uint32_t gen;
    bool playlist;
    char *text;
    int len;
};
static int receive_body(void *arg, const uint8_t *p, int n)
{
    struct transfer *t = arg;
    if (!current(t->gen)) return CANCELLED;
    if (t->playlist) {
        if (n > PLAYLIST_MAX - t->len) return WE_TOO_BIG;
        memcpy(t->text + t->len, p, (size_t)n);
        t->len += n;
        t->text[t->len] = 0;
        return 0;
    }
    while (n) {
        gz_mutex_lock(&lock);
        if (!requested || generation != t->gen) { gz_mutex_unlock(&lock); return CANCELLED; }
        uint32_t take = MIN((uint32_t)n, MIN(RADIO_CAPACITY - used, RADIO_CAPACITY - write_pos));
        if (take) {
            memcpy(ring + write_pos, p, take);
            write_pos = (write_pos + take) % RADIO_CAPACITY;
            used += take;
        }
        gz_mutex_unlock(&lock);
        if (!take) gz_sleep_ms(10);
        p += take; n -= (int)take;
    }
    return 0;
}
static void receive_title(void *arg, const char *title)
{
    struct transfer *t = arg;
    gz_mutex_lock(&lock);
    if (requested && generation == t->gen)
        snprintf(state.title, sizeof(state.title), "%s", title);
    gz_mutex_unlock(&lock);
}
static int map_dns(int rv)
{
    return rv == GZ_ENETDOWN ? WE_NETDOWN : rv == GZ_ETIMEDOUT ? WE_TIMEOUT : WE_DNS;
}

/* One response at a time, redirects bounded independently of station retry.
 * Audio bodies go directly to the bounded producer buffer, not a file. */
static int transfer(const char *address, struct transfer *t)
{
    struct radio_http *h = calloc(1, sizeof(*h));
    struct url *u = malloc(sizeof(*u));
    if (!h || !u) { free(h); free(u); return WE_NOMEM; }
    char url[RADIO_URL_MAX];
    snprintf(url, sizeof(url), "%s", address);
    int result = WE_REDIRECTS;
    for (int redirects = 0; redirects <= 5 && current(t->gen); redirects++) {
        if (!url_parse(url, u) || !u->host[0] || !u->has_auth ||
            (strcmp(u->scheme, "https") && strcmp(u->scheme, "http"))) { result = WE_BADURL; break; }
        uint32_t addr = 0;
        int rv = net_resolve(u->host, &addr, 2000);
        if (!current(t->gen)) { result = CANCELLED; break; }
        if (rv < 0) { result = map_dns(rv); break; }
        struct conn c;
        int port = u->port > 0 ? u->port : url_default_port(u->scheme);
        rv = conn_open(&c, addr, port, !strcmp(u->scheme, "https") ? u->host : NULL);
        uint32_t last = gz_time_ms();
        while (rv == 0 && current(t->gen)) {
            rv = conn_ready(&c);
            if (rv) break;
            if (gz_time_ms() - last > IDLE_MS) { rv = WE_TIMEOUT; break; }
            gz_sleep_ms(10);
        }
        if (!current(t->gen)) rv = CANCELLED;
        if (rv < 0) { conn_close(&c); result = rv; break; }
        char request_text[4096], host[160];
        if (port != url_default_port(u->scheme)) snprintf(host, sizeof(host), "%s:%d", u->host, port);
        else snprintf(host, sizeof(host), "%s", u->host);
        int len = snprintf(request_text, sizeof(request_text),
            "GET %s%s%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Glaze-SomaFM/1.0\r\n"
            "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n%s\r\n",
            u->path[0] ? u->path : "/", u->has_query ? "?" : "", u->query, host,
            t->playlist ? "" : "Icy-MetaData: 1\r\n");
        if (len >= (int)sizeof(request_text)) { conn_close(&c); result = WE_BADURL; break; }
        int sent = 0;
        last = gz_time_ms();
        while (sent < len && current(t->gen)) {
            rv = conn_write(&c, request_text + sent, len - sent);
            if (rv == -CONN_AGAIN) {
                if (gz_time_ms() - last > IDLE_MS) { rv = WE_TIMEOUT; break; }
                gz_sleep_ms(10); continue;
            }
            if (rv <= 0) break;
            sent += rv; last = gz_time_ms();
        }
        if (!current(t->gen)) { conn_close(&c); result = CANCELLED; break; }
        if (sent != len) { conn_close(&c); result = rv < 0 ? rv : WE_RESET; break; }
        radio_http_init(h, receive_body, t->playlist ? NULL : receive_title, t);
        uint8_t data[4096];
        last = gz_time_ms();
        rv = 0;
        while (current(t->gen)) {
            int n = conn_read(&c, data, sizeof(data));
            if (n == -CONN_AGAIN) {
                if (gz_time_ms() - last > IDLE_MS) { rv = WE_TIMEOUT; break; }
                gz_sleep_ms(10); continue;
            }
            if (n < 0) { rv = n; break; }
            if (!n) { rv = radio_http_eof(h) > 0 ? 1 : WE_RESET; break; }
            rv = radio_http_feed(h, data, n);
            if (rv == -1) rv = WE_PROTOCOL;
            last = gz_time_ms(); /* callback can wait for buffer space */
            if (h->headers && h->status == 200 && !t->playlist && h->type[0] &&
                strncasecmp(h->type, "audio/mpeg", 10) &&
                strncasecmp(h->type, "audio/mp3", 9) &&
                strncasecmp(h->type, "application/octet-stream", 24)) rv = WE_DECODE;
            if (rv) break;
        }
        conn_close(&c);
        if (!current(t->gen)) { result = CANCELLED; break; }
        if (rv < 0) { result = rv; break; }
        if (h->status >= 300 && h->status < 400 && h->location[0]) {
            char *next = url_resolve(url, h->location);
            if (!next || strlen(next) >= sizeof(url) ||
                (!strncmp(url, "https:", 6) && strncmp(next, "https:", 6))) {
                free(next); result = WE_BADURL; break;
            }
            strcpy(url, next); free(next); continue;
        }
        result = h->status == 200 ? 0 : WE_CONNECT;
        break;
    }
    free(u); free(h);
    return result;
}

static bool permanent(int rv)
{
    return rv == WE_CERT || rv == WE_CERT_NAME || rv == WE_CERT_DATE ||
        rv == WE_TLS || rv == WE_DECODE || rv == WE_NOMEM || rv == WE_NETDOWN;
}
static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        gz_sem_wait(wake, -1);
        gz_mutex_lock(&lock);
        uint32_t gen = generation;
        struct radio_request req = request;
        bool active = requested;
        gz_mutex_unlock(&lock);
        if (!active) continue;
        struct radio_playlist *playlist = calloc(1, sizeof(*playlist));
        char *text = malloc(PLAYLIST_MAX + 1);
        int error = WE_NOMEM;
        for (int retry = 0; playlist && text && retry <= 5 && current(gen); retry++) {
            if (retry) {
                static const uint32_t delays[] = {1000, 2000, 4000, 8000, 15000};
                gz_mutex_lock(&lock);
                if (generation == gen) {
                    state.phase = RADIO_RECONNECTING;
                    state.retries = (uint32_t)retry;
                    state.error = public_error(error);
                }
                gz_mutex_unlock(&lock);
                if (!wait_current(gen, delays[retry-1])) break;
            }
            struct transfer t = { .gen = gen, .playlist = true, .text = text };
            text[0] = 0;
            error = transfer(req.playlist, &t);
            if (!error && radio_parse_playlist(text, playlist)) error = WE_PROTOCOL;
            if (error) { if (permanent(error)) break; else continue; }
            for (int i = 0; i < playlist->count && current(gen); i++) {
                gz_mutex_lock(&lock);
                if (generation == gen) {
                    epoch++; used = read_pos = write_pos = 0;
                    state.phase = RADIO_BUFFERING; state.error = RADIO_OK;
                    state.title[0] = 0;
                }
                gz_mutex_unlock(&lock);
                t.playlist = false;
                error = transfer(playlist->urls[i], &t);
                if (!error) error = WE_RESET; /* live broadcasts should not end */
                if (permanent(error)) break;
            }
            if (permanent(error)) break;
        }
        free(text); free(playlist);
        gz_mutex_lock(&lock);
        if (generation == gen && requested) {
            requested = false; used = 0; epoch++;
            state.phase = RADIO_ERROR; state.error = public_error(error);
        }
        gz_mutex_unlock(&lock);
    }
}

int radio_start(const struct radio_request *req)
{
    if (!memchr(req->id, 0, sizeof(req->id)) || !req->id[0] ||
        !memchr(req->name, 0, sizeof(req->name)) || !req->name[0] ||
        !memchr(req->playlist, 0, sizeof(req->playlist)) ||
        (strncmp(req->playlist, "https://", 8) && strncmp(req->playlist, "http://", 7))) return GZ_EINVAL;
    if (!ring) ring = malloc(RADIO_CAPACITY);
    if (!ring) return GZ_ENOMEM;
    if (!wake) {
        wake = gz_sem_create(0);
        if (wake < 0) { wake = 0; return GZ_ENOMEM; }
        if (gz_thread_create(worker, NULL) < 0) {
            gz_sem_destroy(wake); wake = 0; return GZ_ENOMEM;
        }
    }
    gz_mutex_lock(&lock);
    generation++; epoch++; used = read_pos = write_pos = 0;
    request = *req; requested = true;
    memset(&state, 0, sizeof(state));
    state.phase = RADIO_CONNECTING;
    strcpy(state.id, req->id); strcpy(state.name, req->name);
    gz_mutex_unlock(&lock);
    gz_sem_post(wake);
    return 0;
}
void radio_stop(void)
{
    gz_mutex_lock(&lock);
    generation++; epoch++; requested = false; used = 0;
    state.phase = RADIO_STOPPED; state.error = RADIO_OK; state.title[0] = 0;
    gz_mutex_unlock(&lock);
    if (wake) gz_sem_post(wake);
}
void radio_fail(uint32_t error)
{
    radio_stop();
    gz_mutex_lock(&lock);
    state.phase = RADIO_ERROR; state.error = error;
    gz_mutex_unlock(&lock);
}
bool radio_active(void)
{
    gz_mutex_lock(&lock);
    bool active = requested;
    gz_mutex_unlock(&lock);
    return active;
}
void radio_snapshot(struct radio_state *out)
{
    gz_mutex_lock(&lock);
    *out = state; out->buffered_bytes = used;
    gz_mutex_unlock(&lock);
}
uint32_t radio_buffered(uint32_t *out_epoch)
{
    gz_mutex_lock(&lock);
    *out_epoch = epoch;
    uint32_t n = used;
    gz_mutex_unlock(&lock);
    return n;
}
int radio_read(uint32_t expected, uint8_t *data, int n)
{
    gz_mutex_lock(&lock);
    if (expected != epoch || !requested) n = 0;
    else {
        n = (int)MIN((uint32_t)n, MIN(used, RADIO_CAPACITY-read_pos));
        memcpy(data, ring + read_pos, (size_t)n);
        read_pos = (read_pos + (uint32_t)n) % RADIO_CAPACITY;
        used -= (uint32_t)n;
    }
    gz_mutex_unlock(&lock);
    return n;
}
void radio_output(bool playing, uint32_t bitrate)
{
    gz_mutex_lock(&lock);
    if (requested && (state.phase == RADIO_PLAYING || state.phase == RADIO_BUFFERING))
        state.phase = playing ? RADIO_PLAYING : RADIO_BUFFERING;
    if (bitrate) state.bitrate = bitrate;
    gz_mutex_unlock(&lock);
}
