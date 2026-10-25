#include "radio_protocol.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) s[--n] = 0;
    return s;
}

int radio_parse_playlist(char *text, struct radio_playlist *p)
{
    p->count = 0;
    bool pls = false;
    for (char *s = text; s && *s;) {
        char *next = strchr(s, '\n');
        if (next) *next++ = 0;
        s = trim(s);
        if (!strcasecmp(s, "[playlist]")) pls = true;
        else if (pls && !strncasecmp(s, "File", 4) && isdigit((unsigned char)s[4])) {
            char *q = s + 4;
            while (isdigit((unsigned char)*q)) q++;
            if (*q == '=') {
                char *url = trim(q + 1);
                size_t n = strlen(url);
                if ((!strncmp(url, "https://", 8) || !strncmp(url, "http://", 7)) &&
                    n < RADIO_URL_MAX && p->count < RADIO_SERVERS) {
                    bool duplicate = false;
                    for (int i = 0; i < p->count; i++)
                        if (!strcmp(p->urls[i], url)) duplicate = true;
                    if (!duplicate) strcpy(p->urls[p->count++], url);
                }
            }
        }
        s = next;
    }
    return p->count ? 0 : -1;
}

void radio_http_init(struct radio_http *h, radio_body_fn body, radio_title_fn title, void *ctx)
{
    memset(h, 0, sizeof(*h));
    h->remaining = -1;
    h->body = body;
    h->title = title;
    h->ctx = ctx;
}

static int headers(struct radio_http *h)
{
    char *s = h->head, *end = strstr(s, "\r\n");
    if (!end) return -1;
    *end = 0;
    char *code = strchr(s, ' ');
    if ((!strncmp(s, "HTTP/1.0 ", 9) || !strncmp(s, "HTTP/1.1 ", 9) || !strncmp(s, "ICY ", 4)) &&
        code && strlen(code + 1) >= 3 && isdigit((unsigned char)code[1]) &&
        isdigit((unsigned char)code[2]) && isdigit((unsigned char)code[3]) &&
        (!code[4] || code[4] == ' '))
        h->status = (code[1]-'0')*100 + (code[2]-'0')*10 + code[3]-'0';
    else return -1;
    bool have_length = false;
    for (s = end + 2; *s; s = end + 2) {
        end = strstr(s, "\r\n");
        if (!end) return -1;
        if (end == s) break;
        *end = 0;
        char *colon = strchr(s, ':');
        if (!colon) return -1;
        *colon = 0;
        char *v = trim(colon + 1);
        if (!strcasecmp(s, "Content-Length")) {
            if (have_length || !*v) return -1;
            uint32_t length = 0;
            for (char *p = v; *p; p++) {
                if (!isdigit((unsigned char)*p) || length > (0x7fffffffu - (*p-'0'))/10) return -1;
                length = length*10 + *p-'0';
            }
            h->remaining = length;
            have_length = true;
        } else if (!strcasecmp(s, "Transfer-Encoding")) {
            if (strcasecmp(v, "chunked") || h->chunked) return -1;
            h->chunked = true;
        } else if (!strcasecmp(s, "Content-Encoding")) {
            if (strcasecmp(v, "identity")) return -1;
        } else if (!strcasecmp(s, "Location")) {
            if (strlen(v) >= sizeof(h->location)) return -1;
            strcpy(h->location, v);
        } else if (!strcasecmp(s, "Content-Type")) {
            if (strlen(v) >= sizeof(h->type)) return -1;
            strcpy(h->type, v);
        } else if (!strcasecmp(s, "icy-metaint")) {
            uint32_t interval = 0;
            if (!*v) return -1;
            for (char *p = v; *p; p++) {
                if (!isdigit((unsigned char)*p) || interval > (0x7fffffffu - (*p-'0'))/10) return -1;
                interval = interval*10 + *p-'0';
            }
            h->metaint = h->audio_left = interval;
        }
    }
    if (h->chunked && have_length) return -1;
    if (h->status >= 100 && h->status < 200) {
        if (h->status == 101) return -1;
        radio_body_fn body = h->body;
        radio_title_fn title = h->title;
        void *ctx = h->ctx;
        radio_http_init(h, body, title, ctx);
        return 0;
    }
    h->headers = true;
    if ((h->status >= 300 && h->status < 400) || h->status != 200 ||
        (!h->chunked && h->remaining == 0)) h->done = true;
    return 0;
}

static void metadata(struct radio_http *h)
{
    h->meta[h->meta_len] = 0;
    const char *s = strstr(h->meta, "StreamTitle='");
    if (!s || !h->title) return;
    s += 13;
    const char *end = strstr(s, "';");
    if (!end) return;
    char title[192];
    size_t n = (size_t)(end-s);
    if (n >= sizeof(title)) n = sizeof(title)-1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        title[i] = c >= 32 && c < 127 ? (char)c : ' ';
    }
    title[n] = 0;
    h->title(h->ctx, title);
}

static int body(struct radio_http *h, const uint8_t *p, int n)
{
    if (!h->metaint) return h->body(h->ctx, p, n);
    while (n) {
        if (h->audio_left) {
            int take = h->audio_left < (uint32_t)n ? (int)h->audio_left : n;
            int rv = h->body(h->ctx, p, take);
            if (rv) return rv;
            p += take; n -= take; h->audio_left -= take;
        } else if (!h->meta_left) {
            h->meta_left = (uint32_t)*p++ * 16;
            n--; h->meta_len = 0;
            if (!h->meta_left) h->audio_left = h->metaint;
        } else {
            int take = h->meta_left < (uint32_t)n ? (int)h->meta_left : n;
            memcpy(h->meta + h->meta_len, p, (size_t)take);
            h->meta_len += take; h->meta_left -= take;
            p += take; n -= take;
            if (!h->meta_left) { metadata(h); h->audio_left = h->metaint; }
        }
    }
    return 0;
}

int radio_http_feed(struct radio_http *h, const uint8_t *p, int n)
{
    while (n && !h->done) {
        if (!h->headers) {
            if (h->head_len >= (int)sizeof(h->head)-1 || !*p) return -1;
            h->head[h->head_len++] = (char)*p++; n--;
            h->head[h->head_len] = 0;
            if (h->head_len >= 4 && !memcmp(h->head+h->head_len-4, "\r\n\r\n", 4)) {
                int rv = headers(h);
                if (rv) return rv;
            }
        } else if (!h->chunked) {
            int take = n;
            if (h->remaining >= 0 && h->remaining < take) take = (int)h->remaining;
            int rv = body(h, p, take);
            if (rv) return rv;
            p += take; n -= take;
            if (h->remaining >= 0) {
                h->remaining -= take;
                if (!h->remaining) h->done = true;
            }
        } else if (h->chunk_state == 1) {
            int take = h->chunk_left < (uint32_t)n ? (int)h->chunk_left : n;
            int rv = body(h, p, take);
            if (rv) return rv;
            p += take; n -= take; h->chunk_left -= take;
            if (!h->chunk_left) h->chunk_state = 2;
        } else if (h->chunk_state == 2 || h->chunk_state == 3) {
            if (*p++ != (h->chunk_state == 2 ? '\r' : '\n')) return -1;
            n--;
            h->chunk_state = h->chunk_state == 2 ? 3 : 0;
        } else {
            if (h->line_len >= (int)sizeof(h->line)-1 || !*p) return -1;
            h->line[h->line_len++] = (char)*p++; n--;
            if (h->line_len >= 2 && h->line[h->line_len-2] == '\r' && h->line[h->line_len-1] == '\n') {
                h->line[h->line_len-2] = 0;
                if (h->chunk_state == 4) {
                    if (h->line_len == 2) h->done = true;
                } else {
                    uint32_t size = 0;
                    char *s = h->line;
                    if (!isxdigit((unsigned char)*s)) return -1;
                    for (; isxdigit((unsigned char)*s); s++) {
                        int digit = isdigit((unsigned char)*s) ? *s-'0' : tolower((unsigned char)*s)-'a'+10;
                        if (size > (0x7fffffffu-digit)/16) return -1;
                        size = size*16 + digit;
                    }
                    if (*s && *s != ';') return -1;
                    h->chunk_left = size;
                    h->chunk_state = size ? 1 : 4;
                }
                h->line_len = 0;
            }
        }
    }
    return h->done ? 1 : 0;
}

int radio_http_eof(const struct radio_http *h)
{
    return h->done || (h->headers && !h->chunked && h->remaining < 0 && !h->meta_left) ? 1 : -1;
}
