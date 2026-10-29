#include "json.h"
#include <string.h>
static void space(const struct cj *j, int *p)
{
    while (*p < j->len && (j->data[*p] == ' ' || j->data[*p] == '\n' || j->data[*p] == '\r' || j->data[*p] == '\t')) (*p)++;
}
static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool string_end(const struct cj *j, int *p)
{
    (*p)++;
    while (*p < j->len) {
        unsigned char c = (unsigned char)j->data[(*p)++];
        if (c == '"') return true;
        if (c < 32) return false;
        if (c == '\\') {
            if (*p == j->len) return false;
            char e = j->data[(*p)++];
            if (e == 'u') {
                if (j->len - *p < 4) return false;
                for (int i = 0; i < 4; i++) if (hex(j->data[(*p)++]) < 0) return false;
            } else if (!strchr("\"\\/bfnrt", e)) return false;
        }
    }
    return false;
}
static int value(struct cj *j, int *p, int depth)
{
    space(j, p);
    if (*p == j->len || depth > 20 || j->count == CJ_MAX) return -1;
    int id = j->count++;
    j->nodes[id].start = *p;
    char c = j->data[*p];
    if (c == '{' || c == '[') {
        bool object = c == '{'; char end = object ? '}' : ']';
        j->nodes[id].kind = object ? CJ_OBJECT : CJ_ARRAY;
        (*p)++; space(j, p);
        if (*p < j->len && j->data[*p] == end) (*p)++;
        else for (;;) {
            if (object) {
                space(j, p);
                if (*p == j->len || j->data[*p] != '"' || value(j, p, depth + 1) < 0) return -1;
                space(j, p);
                if (*p == j->len || j->data[(*p)++] != ':') return -1;
            }
            if (value(j, p, depth + 1) < 0) return -1;
            space(j, p);
            if (*p == j->len) return -1;
            c = j->data[(*p)++];
            if (c == end) break;
            if (c != ',') return -1;
        }
    } else if (c == '"') {
        j->nodes[id].kind = CJ_STRING;
        if (!string_end(j, p)) return -1;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        j->nodes[id].kind = CJ_NUMBER;
        if (c == '-') (*p)++;
        if (*p == j->len) return -1;
        if (j->data[*p] == '0') (*p)++;
        else {
            if (j->data[*p] < '1' || j->data[*p] > '9') return -1;
            while (*p < j->len && j->data[*p] >= '0' && j->data[*p] <= '9') (*p)++;
        }
        if (*p < j->len && j->data[*p] == '.') {
            int first = ++*p;
            while (*p < j->len && j->data[*p] >= '0' && j->data[*p] <= '9') (*p)++;
            if (*p == first) return -1;
        }
        if (*p < j->len && (j->data[*p] == 'e' || j->data[*p] == 'E')) {
            (*p)++;
            if (*p < j->len && (j->data[*p] == '-' || j->data[*p] == '+')) (*p)++;
            int first = *p;
            while (*p < j->len && j->data[*p] >= '0' && j->data[*p] <= '9') (*p)++;
            if (*p == first) return -1;
        }
    } else {
        j->nodes[id].kind = CJ_LITERAL;
        const char *s = c == 't' ? "true" : c == 'f' ? "false" : c == 'n' ? "null" : "";
        int n = (int)strlen(s);
        if (!n || j->len - *p < n || memcmp(j->data + *p, s, (size_t)n)) return -1;
        *p += n;
    }
    j->nodes[id].end = *p; j->nodes[id].next = j->count;
    return id;
}
bool cj_parse(struct cj *j, const void *data, size_t len)
{
    if (len > 1024 * 1024 || !len) return false;
    j->data = data; j->len = (int)len; j->count = 0;
    int p = 0;
    if (value(j, &p, 0) != 0) return false;
    space(j, &p); return p == j->len;
}
int cj_string(const struct cj *j, int node, char *out, size_t cap)
{
    if (node < 0 || node >= j->count || j->nodes[node].kind != CJ_STRING || !cap) return -1;
    int p = j->nodes[node].start + 1, end = j->nodes[node].end - 1; size_t used = 0;
    while (p < end) {
        unsigned char c = (unsigned char)j->data[p++];
        uint32_t code = c;
        bool unicode = false;
        if (c == '\\') {
            c = (unsigned char)j->data[p++];
            if (c == 'u') {
                code = 0;
                for (int i = 0; i < 4; i++) code = code * 16 + (unsigned)hex(j->data[p++]);
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (end - p < 6 || j->data[p++] != '\\' || j->data[p++] != 'u') return -1;
                    uint32_t low = 0;
                    for (int i = 0; i < 4; i++) { int h = hex(j->data[p++]); if (h < 0) return -1; low = low * 16 + (unsigned)h; }
                    if (low < 0xdc00 || low > 0xdfff) return -1;
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                } else if (code >= 0xdc00 && code <= 0xdfff) return -1;
                unicode = true;
            } else {
                const char *names = "bfnrt", *values = "\b\f\n\r\t", *at = strchr(names, c);
                code = at ? (unsigned char)values[at - names] : c;
            }
        }
        if (!code) return -1; /* C strings cannot represent JSON NUL. */
        unsigned char bytes[4]; int n = 1;
        if (!unicode || code < 0x80) bytes[0] = (unsigned char)code;
        else if (code < 0x800) { bytes[0] = 0xc0 | (code >> 6); bytes[1] = 0x80 | (code & 63); n = 2; }
        else if (code < 0x10000) { bytes[0] = 0xe0 | (code >> 12); bytes[1] = 0x80 | ((code >> 6) & 63); bytes[2] = 0x80 | (code & 63); n = 3; }
        else { bytes[0] = 0xf0 | (code >> 18); bytes[1] = 0x80 | ((code >> 12) & 63); bytes[2] = 0x80 | ((code >> 6) & 63); bytes[3] = 0x80 | (code & 63); n = 4; }
        if (used + (size_t)n >= cap) return -1;
        memcpy(out + used, bytes, (size_t)n); used += (size_t)n;
    }
    out[used] = 0; return (int)used;
}
int cj_find(const struct cj *j, int object, const char *key)
{
    if (object < 0 || object >= j->count || j->nodes[object].kind != CJ_OBJECT) return -2;
    int found = -1; char name[128];
    for (int i = object + 1; i < j->nodes[object].next;) {
        int v = i + 1;
        if (cj_string(j, i, name, sizeof(name)) >= 0 && !strcmp(key, name)) {
            if (found >= 0) return -2;
            found = v;
        }
        i = j->nodes[v].next;
    }
    return found;
}
bool cj_u32(const struct cj *j, int node, uint32_t *out)
{
    if (node < 0 || node >= j->count || (j->nodes[node].kind != CJ_NUMBER && j->nodes[node].kind != CJ_STRING)) return false;
    int p = j->nodes[node].start, end = j->nodes[node].end;
    if (j->nodes[node].kind == CJ_STRING) { p++; end--; }
    if (p == end) return false;
    uint32_t n = 0;
    for (; p < end; p++) {
        unsigned c = (unsigned char)j->data[p] - '0';
        if (c > 9 || n > (0xffffffffu - c) / 10) return false;
        n = n * 10 + c;
    }
    *out = n; return true;
}
bool cj_is(const struct cj *j, int node, const char *literal)
{
    return node >= 0 && node < j->count && j->nodes[node].kind == CJ_LITERAL &&
           (size_t)(j->nodes[node].end - j->nodes[node].start) == strlen(literal) &&
           !memcmp(j->data + j->nodes[node].start, literal, strlen(literal));
}
