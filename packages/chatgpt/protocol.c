#include "chat.h"
#include "codex.h"
#include <stdio.h>
#include <string.h>

static bool valid_text(const char *s, size_t max)
{
    for (size_t i = 0; i < max; i++) {
        if (!s[i]) return true;
        if ((unsigned char)s[i] < 32 && s[i] != '\n' && s[i] != '\t') return false;
    }
    return false;
}
bool chat_history_valid(const struct chat_history *h)
{
    if (h->version != 1 || h->count > CHAT_COUNT) return false;
    for (unsigned i = 0; i < h->count; i++)
        if (h->messages[i].assistant != (i & 1u) || !h->messages[i].text[0] ||
            !valid_text(h->messages[i].text, CHAT_TEXT)) return false;
    return true;
}
bool chat_append(struct chat_history *h, bool assistant, const char *text)
{
    if (!chat_history_valid(h) || assistant != (bool)(h->count & 1) || !*text ||
        !valid_text(text, CHAT_TEXT)) return false;
    if (h->count == CHAT_COUNT) {
        memmove(h->messages, h->messages + 2, (CHAT_COUNT - 2) * sizeof(h->messages[0]));
        h->count -= 2;
    }
    struct chat_message *m = &h->messages[h->count++];
    memset(m, 0, sizeof(*m));
    m->assistant = assistant;
    memcpy(m->text, text, strlen(text) + 1);
    return true;
}
static bool put(char **out, size_t *left, const char *s, size_t len)
{
    if (len >= *left) return false;
    memcpy(*out, s, len); *out += len; *left -= len; **out = 0;
    return true;
}
static int request(const struct chat_history *h, const char *model, char *out, size_t cap)
{
    if (!chat_history_valid(h) || !(h->count & 1) || !cap) return -1;
    char *p = out;
    size_t left = cap;
    if (model) {
        if (!chat_model_ok(model)) return -1;
        const char *prefix = "{\"store\":false,\"stream\":true,\"tools\":[],\"instructions\":\"Answer clearly and concisely for a small handheld display. Use plain text.\",\"model\":\"";
        if (!put(&p, &left, prefix, strlen(prefix)) || !put(&p, &left, model, strlen(model)) ||
            !put(&p, &left, "\",\"input\":[", 11)) return -1;
    } else if (!put(&p, &left, "{\"messages\":[", 13)) return -1;
    for (unsigned i = 0; i < h->count; i++) {
        const char *prefix = model ? (h->messages[i].assistant ?
            "{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"" :
            "{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"") :
            (h->messages[i].assistant ? "{\"role\":\"assistant\",\"content\":\"" : "{\"role\":\"user\",\"content\":\"");
        if ((i && !put(&p, &left, ",", 1)) || !put(&p, &left, prefix, strlen(prefix))) return -1;
        for (const unsigned char *s = (const unsigned char *)h->messages[i].text; *s; s++) {
            char buf[2]; size_t n;
            if (*s == '"' || *s == '\\' || *s == '\n' || *s == '\t') {
                buf[0] = '\\'; buf[1] = *s == '\n' ? 'n' : *s == '\t' ? 't' : (char)*s; n = 2;
            } else if (*s >= 128) {
                buf[0] = (char)(0xc0 | (*s >> 6)); buf[1] = (char)(0x80 | (*s & 63)); n = 2;
            } else { buf[0] = (char)*s; n = 1; }
            if (!put(&p, &left, buf, n)) return -1;
        }
        if (!put(&p, &left, model ? "\"}]}" : "\"}", model ? 4 : 2)) return -1;
    }
    if (!put(&p, &left, "]}", 2)) return -1;
    return (int)(p - out);
}
int chat_request(const struct chat_history *h, char *out, size_t cap) { return request(h, NULL, out, cap); }
int chat_codex_request(const struct chat_history *h, const char *model, char *out, size_t cap) { return request(h, model, out, cap); }
bool chat_model_ok(const char *s)
{
    size_t n = strnlen(s, 64);
    if (!n || n == 64) return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '.' || s[i] == '_')) return false;
    return true;
}
bool chat_text(const uint8_t *data, size_t len, char *out, size_t cap)
{
    if (cap < 32) return false;
    size_t used = 0;
    bool truncated = false;
    unsigned newlines = 0;
    for (size_t i = 0; i < len;) {
        uint32_t c = data[i++]; int more = 0; uint32_t min = 0;
        if (c >= 0xf0 && c <= 0xf4) { c &= 7; more = 3; min = 0x10000; }
        else if (c >= 0xe0 && c <= 0xef) { c &= 15; more = 2; min = 0x800; }
        else if (c >= 0xc2 && c <= 0xdf) { c &= 31; more = 1; min = 0x80; }
        else if (c >= 0x80) return false;
        if (i + (size_t)more > len) return false;
        for (int j = 0; j < more; j++) {
            if ((data[i] & 0xc0) != 0x80) return false;
            c = c << 6 | (data[i++] & 63);
        }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) || c == 0) return false;
        if (c == '\r') continue;
        if (c == '\t') c = ' ';
        if (c < 32 && c != '\n') return false;
        if (c == 0x2018 || c == 0x2019) c = '\'';
        if (c == 0x201c || c == 0x201d) c = '"';
        if (c == 0x2013 || c == 0x2014) c = '-';
        if (c == 0x2022) c = '*';
        if (c > 255 || (c >= 127 && c < 160)) c = '?';
        if (c == '\n') newlines++;
        if (used + 24 < cap && newlines < 128) out[used++] = (char)c;
        else truncated = true;
    }
    if (truncated) {
        const char *suffix = "\n[Reply shortened]";
        memcpy(out + used, suffix, strlen(suffix)); used += strlen(suffix);
    }
    out[used] = 0;
    return used > 0;
}
