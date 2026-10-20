/*
 * Message text: AIM clients send a small HTML document per IM (and for
 * profiles and away messages); the app shows plain Latin-1, which is what
 * the fonts cover.
 */

#include <ctype.h>
#include <string.h>
#include "oscar.h"

static const struct {
    const char *name;
    uint8_t ch;
} entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
    { "nbsp", ' ' }, { "copy", 0xA9 }, { "reg", 0xAE }, { "eacute", 0xE9 },
};

/* A character for code point c: Latin-1 as is, typographic punctuation
 * as ASCII (the fonts keep 0x80-0x9F for their own symbols), else '?'. */
static int latin1(unsigned c)
{
    switch (c) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212: return '-';
    case 0x2022: return 0x83;   /* GFX_CH_BULLET */
    case 0x2026: return 0x84;   /* GFX_CH_ELLIPSIS */
    case 0xA0: return ' ';
    }
    return c < 0x20 || (c >= 0x7F && c < 0xA0) || c > 0xFF ? '?' : (int)c;
}

/* Windows-1252's 0x80-0x9F, which Windows clients send as "Latin-1". */
static unsigned cp1252(unsigned char c)
{
    static const uint16_t map[32] = {
        0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
        0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E,
        0x0178,
    };
    return c >= 0x80 && c < 0xA0 ? map[c - 0x80] : c;
}

/* A tag name at s (after '<' and any '/'), lowercased into out. */
static void tag_name(const char *s, const char *end, char *out, size_t n)
{
    size_t i = 0;

    while (s < end && *s == '/')
        s++;
    while (s < end && isalnum((unsigned char)*s) && i + 1 < n)
        out[i++] = (char)tolower((unsigned char)*s++);
    out[i] = 0;
}

/* Decodes the entity at s ('&' ... ';'); returns its length or 0. */
static size_t entity(const char *s, const char *end, int *ch)
{
    const char *semi = s + 1;

    while (semi < end && semi - s < 10 && *semi != ';')
        semi++;
    if (semi >= end || *semi != ';')
        return 0;
    if (s[1] == '#') {
        bool hex = s + 2 < semi && (s[2] == 'x' || s[2] == 'X');
        int v = 0;
        for (const char *p = s + (hex ? 3 : 2); p < semi; p++) {
            int d = isdigit((unsigned char)*p) ? *p - '0'
                  : hex && isxdigit((unsigned char)*p) ? (tolower((unsigned char)*p) - 'a' + 10) : -1;
            if (d < 0 || v > 0x10FFFF)
                return 0;
            v = v * (hex ? 16 : 10) + d;
        }
        *ch = v < 0x20 ? ' ' : latin1((unsigned)v);
        return (size_t)(semi - s + 1);
    }
    for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); i++) {
        size_t n = strlen(entities[i].name);
        if ((size_t)(semi - s - 1) == n && !strncmp(s + 1, entities[i].name, n)) {
            *ch = entities[i].ch;
            return n + 2;
        }
    }
    return 0;
}

void oscar_html_to_text(char *out, size_t n, const uint8_t *in, size_t len, uint16_t charset)
{
    char latin[OSCAR_MSG_MAX * 2];
    size_t ln = 0, o = 0;
    bool space = false;     /* a collapsed space is waiting */

    if (!n)
        return;
    /* Charset 2 is UCS-2BE; anything outside Latin-1 becomes '?'. */
    if (charset == 2) {
        for (size_t i = 0; i + 1 < len && ln < sizeof(latin); i += 2) {
            unsigned c = (unsigned)in[i] << 8 | in[i + 1];
            latin[ln++] = (char)(c == '\n' || c == '\r' || c == '\t' ? (int)c : latin1(c));
        }
    } else {
        ln = len < sizeof(latin) ? len : sizeof(latin);
        for (size_t i = 0; i < ln; i++) {
            unsigned char c = in[i];
            latin[i] = (char)(c >= 0x80 && c < 0xA0 ? latin1(cp1252(c)) : c);
        }
    }

    const char *s = latin, *end = latin + ln;
    while (s < end && o + 1 < n) {
        int ch = (unsigned char)*s;
        if (ch == '<') {
            const char *close = memchr(s, '>', (size_t)(end - s));
            char name[8];
            if (!close)
                break;
            tag_name(s + 1, close, name, sizeof(name));
            if (!strcmp(name, "br") || (!strcmp(name, "p") && s[1] == '/') || !strcmp(name, "div")) {
                if (o && out[o - 1] != '\n')
                    out[o++] = '\n';
                else if (!strcmp(name, "br"))
                    out[o++] = '\n';
                space = false;
            }
            s = close + 1;
            continue;
        }
        size_t used = 1;
        if (ch == '&') {
            size_t k = entity(s, end, &ch);
            if (k)
                used = k;
        } else if (isspace(ch)) {
            space = o > 0 && out[o - 1] != '\n';
            s++;
            continue;
        } else if (ch < 0x20 || ch == 0x7F) {
            ch = '?';
        }
        /* Entities and UCS-2 may have produced the font's own symbols on
         * purpose; raw bytes 0x80-0x9F were mapped above. */
        if (space && o + 2 < n)
            out[o++] = ' ';
        space = false;
        if (ch == '\n' && o && out[o - 1] == ' ')
            o--;
        out[o++] = (char)ch;
        s += used;
    }
    while (o && (out[o - 1] == '\n' || out[o - 1] == ' '))
        o--;
    out[o] = 0;
}

size_t oscar_text_to_html(char *out, size_t n, const char *text)
{
    static const char head[] = "<HTML><BODY>", tail[] = "</BODY></HTML>";
    size_t o = 0;

    if (n < sizeof(head) + sizeof(tail))
        return 0;
    memcpy(out, head, sizeof(head) - 1);
    o = sizeof(head) - 1;
    for (const char *p = text; *p; p++) {
        const char *rep = *p == '<' ? "&lt;" : *p == '>' ? "&gt;" : *p == '&' ? "&amp;"
                        : *p == '"' ? "&quot;" : *p == '\n' ? "<BR>" : NULL;
        size_t k = rep ? strlen(rep) : 1;
        if (o + k + sizeof(tail) > n)
            break;
        if (rep)
            memcpy(out + o, rep, k);
        else
            out[o] = *p;
        o += k;
    }
    memcpy(out + o, tail, sizeof(tail));
    return o + sizeof(tail) - 1;
}
