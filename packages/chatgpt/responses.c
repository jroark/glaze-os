#include "codex.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CX_FINISHED (-2001)
struct stream {
    struct cx_io *io;
    struct cj *json;
    char *line, *event, *decoded, *raw;
    size_t line_n, event_n, raw_n;
    bool completed, failed, partial, truncated;
    uint32_t last;
};
static void append(struct stream *s, const char *text, size_t n)
{
    size_t left = CHAT_RESPONSE - 64 - s->raw_n;
    if (n > left) {
        n = left;
        /* Avoid truncating in the middle of a UTF-8 code point. */
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) n--;
        s->truncated = true;
    }
    memcpy(s->raw + s->raw_n, text, n); s->raw_n += n; s->raw[s->raw_n] = 0;
}
static bool fallback(struct stream *s)
{
    int response = cj_find(s->json, 0, "response"), output = cj_find(s->json, response, "output");
    if (output < 0 || s->json->nodes[output].kind != CJ_ARRAY) return false;
    for (int i = output + 1; i < s->json->nodes[output].next; i = s->json->nodes[i].next) {
        char type[64];
        if (cj_string(s->json, cj_find(s->json, i, "type"), type, sizeof(type)) < 0 || strcmp(type, "message")) continue;
        int content = cj_find(s->json, i, "content");
        if (content < 0 || s->json->nodes[content].kind != CJ_ARRAY) return false;
        for (int j = content + 1; j < s->json->nodes[content].next; j = s->json->nodes[j].next) {
            if (cj_string(s->json, cj_find(s->json, j, "type"), type, sizeof(type)) < 0) return false;
            const char *field = !strcmp(type, "output_text") ? "text" : !strcmp(type, "refusal") ? "refusal" : NULL;
            if (!field) continue;
            int n = cj_string(s->json, cj_find(s->json, j, field), s->decoded, CX_EVENT_MAX);
            if (n < 0) return false;
            append(s, s->decoded, (size_t)n);
        }
    }
    return true;
}
static int event(struct stream *s)
{
    if (!s->event_n) return 0;
    s->event[s->event_n] = 0;
    if (!strcmp(s->event, "[DONE]")) return 0;
    if (!cj_parse(s->json, s->event, s->event_n)) return -1;
    char type[80];
    if (cj_string(s->json, cj_find(s->json, 0, "type"), type, sizeof(type)) < 0) return -1;
    if (!strcmp(type, "response.output_text.delta") || !strcmp(type, "response.refusal.delta")) {
        int n = cj_string(s->json, cj_find(s->json, 0, "delta"), s->decoded, CX_EVENT_MAX);
        if (n < 0) return -1;
        append(s, s->decoded, (size_t)n);
        if (s->io->progress && s->io->millis(s->io->arg) - s->last >= 80) {
            char display[CHAT_TEXT];
            if (!chat_text((const uint8_t *)s->raw, s->raw_n, display, sizeof(display))) return -1;
            s->io->progress(s->io->arg, display, false); s->last = s->io->millis(s->io->arg);
        }
    } else if (!strcmp(type, "response.completed") || !strcmp(type, "response.incomplete")) {
        s->partial = !strcmp(type, "response.incomplete");
        if (!s->raw_n && !fallback(s)) return -1;
        int r = cj_find(s->json, 0, "response"), status = cj_find(s->json, r, "status");
        if (status != -1) {
            char value[32];
            if (cj_string(s->json, status, value, sizeof(value)) < 0 || strcmp(value, s->partial ? "incomplete" : "completed")) return -1;
        }
        s->completed = true; return CX_FINISHED;
    } else if (!strcmp(type, "response.failed") || !strcmp(type, "error")) {
        s->failed = true; return CX_FINISHED;
    }
    return 0;
}
static int line(struct stream *s)
{
    if (!s->line_n) { int rv = event(s); s->event_n = 0; return rv; }
    if (s->line_n >= 5 && !memcmp(s->line, "data:", 5)) {
        size_t start = 5;
        if (start < s->line_n && s->line[start] == ' ') start++;
        size_t n = s->line_n - start;
        if (s->event_n + n + 2 >= CX_EVENT_MAX) return -1;
        if (s->event_n) s->event[s->event_n++] = '\n';
        memcpy(s->event + s->event_n, s->line + start, n); s->event_n += n;
    }
    return 0;
}
static int feed(const uint8_t *data, int n, void *arg)
{
    struct stream *s = arg;
    if (!s->io->active(s->io->arg)) return -2;
    for (int i = 0; i < n; i++) {
        if (!data[i]) return -1;
        if (data[i] == '\r') continue;
        if (data[i] == '\n') {
            int rv = line(s); s->line_n = 0;
            if (rv) return rv;
        } else {
            if (s->line_n + 1 >= CX_EVENT_MAX) return -1;
            s->line[s->line_n++] = (char)data[i];
        }
    }
    return 0;
}
static void clear(struct stream *s)
{
    free(s->json); free(s->line); free(s->event); free(s->decoded); free(s->raw);
}
int cx_chat(struct cx_io *io, struct cx_tokens *tokens, const char *body, char *out, size_t cap)
{
    if (!cx_tokens_valid(tokens)) { snprintf(out, cap, "Sign in with ChatGPT first."); return -1; }
    uint32_t now = io->time(io->arg);
    if ((!now || tokens->expires <= now + 60) && cx_refresh(io, tokens, out, cap)) return -1;
    if (!io->save(io->arg, tokens)) { snprintf(out, cap, "Could not save sign-in. Check storage and Retry."); return -1; }
    for (int attempt = 0; attempt < 2; attempt++) {
        struct stream s = {.io = io, .last = io->millis(io->arg)};
        s.json = malloc(sizeof(*s.json)); s.line = malloc(CX_EVENT_MAX); s.event = malloc(CX_EVENT_MAX);
        s.decoded = malloc(CX_EVENT_MAX); s.raw = calloc(1, CHAT_RESPONSE);
        if (!s.json || !s.line || !s.event || !s.decoded || !s.raw) { clear(&s); snprintf(out, cap, "Not enough memory for a reply."); return -1; }
        struct cx_response r = {0};
        int rv = io->post(io, CX_RESPONSES, "application/json", body, tokens->access, tokens->account, feed, &s, &r);
        if (!io->active(io->arg)) { clear(&s); free(r.body); return -2; }
        if (!rv && r.status == 401 && attempt == 0 && !s.raw_n) {
            clear(&s); free(r.body);
            if (cx_refresh(io, tokens, out, cap)) return -1;
            continue;
        }
        bool ok = !rv && r.status == 200 && s.completed && !s.failed && s.raw_n > 0;
        if (ok && (s.partial || s.truncated)) {
            const char *suffix = s.partial ? "\n[Reply incomplete]" : "\n[Reply shortened]";
            size_t n = strlen(suffix); memcpy(s.raw + s.raw_n, suffix, n); s.raw_n += n;
        }
        ok = ok && chat_text((const uint8_t *)s.raw, s.raw_n, out, cap);
        if (!ok) {
            const char *error = r.status == 401 ? "Sign-in was rejected. Sign out and sign in again." :
                r.status == 403 ? "This account or workspace cannot use this Codex model." :
                r.status == 429 ? "Codex usage limit reached. Wait, then Retry." :
                r.status == 400 || r.status == 404 ? "Model unavailable. Choose a Codex model in Account." :
                s.failed ? "Codex could not complete the reply. Retry when ready." :
                "Reply interrupted or invalid. Check the network and clock, then Retry.";
            snprintf(out, cap, "%s", error);
        }
        clear(&s); free(r.body); return ok ? 0 : -1;
    }
    return -1;
}
