/*
 * MIME and RFC 5322 message parsing, encoding, and HTML-to-Latin-1 conversion.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "mail.h"

/* ---- Base64 and Quoted-Printable ---------------------------------------- */

static const char b64_tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t mime_base64_encode(char *out, size_t out_cap, const void *in, size_t in_len)
{
    const uint8_t *p = in;
    size_t o = 0;

    if (!out_cap)
        return 0;
    for (size_t i = 0; i < in_len; i += 3) {
        if (o + 4 >= out_cap)
            break;
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < in_len)
            v |= (uint32_t)p[i + 1] << 8;
        if (i + 2 < in_len)
            v |= p[i + 2];
        out[o++] = b64_tab[(v >> 18) & 0x3F];
        out[o++] = b64_tab[(v >> 12) & 0x3F];
        out[o++] = i + 1 < in_len ? b64_tab[(v >> 6) & 0x3F] : '=';
        out[o++] = i + 2 < in_len ? b64_tab[v & 0x3F] : '=';
    }
    out[o] = 0;
    return o;
}

static int b64_val(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

size_t mime_base64_decode(uint8_t *out, size_t out_cap, const char *in, size_t in_len)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;

    for (size_t i = 0; i < in_len; i++) {
        int c = (unsigned char)in[i];
        if (c == '=')
            break;
        int v = b64_val(c);
        if (v < 0)
            continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o < out_cap)
                out[o++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return o;
}

static int hex_val(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

size_t mime_qp_decode(uint8_t *out, size_t out_cap, const char *in, size_t in_len, bool header_underscores)
{
    size_t o = 0;

    for (size_t i = 0; i < in_len && o < out_cap; i++) {
        char c = in[i];
        if (header_underscores && c == '_') {
            out[o++] = ' ';
            continue;
        }
        if (c == '=') {
            /* Soft line break: = followed by optional horizontal space and CRLF/LF. */
            size_t j = i + 1;
            while (j < in_len && (in[j] == ' ' || in[j] == '\t'))
                j++;
            if (j < in_len && in[j] == '\r') {
                i = (j + 1 < in_len && in[j + 1] == '\n') ? j + 1 : j;
                continue;
            }
            if (j < in_len && in[j] == '\n') {
                i = j;
                continue;
            }
            if (i + 2 < in_len) {
                int hi = hex_val((unsigned char)in[i + 1]);
                int lo = hex_val((unsigned char)in[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    out[o++] = (uint8_t)((hi << 4) | lo);
                    i += 2;
                    continue;
                }
            }
        }
        out[o++] = (uint8_t)c;
    }
    return o;
}

/* ---- Character sets (UTF-8 / Windows-1252 -> Glaze Latin-1) ------------- */

static int codepoint_to_latin1(uint32_t cp)
{
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return '-';
    case 0x2022: case 0x25CF: case 0x25E6: return 0x83; /* GFX_CH_BULLET */
    case 0x2026: return 0x84;                           /* GFX_CH_ELLIPSIS */
    case 0x2190: return 0x85;                           /* GFX_CH_LARROW */
    case 0x2192: return 0x86;                           /* GFX_CH_RARROW */
    case 0x2713: case 0x2714: return 0x87;              /* GFX_CH_CHECK */
    case 0x00A0: case 0x2002: case 0x2003: case 0x2009: return ' ';
    case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: return 0; /* zero-width */
    case 0x2122: return 'T';
    case 0x20AC: return 'E';
    }
    if (cp == '\n' || cp == '\t')
        return (int)cp;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0) || cp > 0xFF)
        return '?';
    return (int)cp;
}

static uint32_t cp1252_to_unicode(uint8_t c)
{
    static const uint16_t map[32] = {
        0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
        0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
    };
    return (c >= 0x80 && c < 0xA0) ? map[c - 0x80] : c;
}

void mime_to_latin1(char *out, size_t out_cap, const uint8_t *in, size_t in_len, const char *charset)
{
    bool force_latin1 = charset && (!strcasecmp(charset, "iso-8859-1") || !strcasecmp(charset, "latin1") ||
                                    !strcasecmp(charset, "windows-1252") || !strcasecmp(charset, "cp1252"));
    size_t o = 0;

    if (!out_cap)
        return;
    for (size_t i = 0; i < in_len && o + 1 < out_cap;) {
        uint8_t b = in[i];
        if (b == '\r') {
            if (i + 1 >= in_len || in[i + 1] != '\n')
                out[o++] = '\n';
            i++;
            continue;
        }
        if (b < 0x80) {
            if (b == '\n' || b == '\t' || b >= 0x20)
                out[o++] = (char)b;
            i++;
            continue;
        }
        if (!force_latin1) {
            /* Decode valid UTF-8 sequences; fall back to CP1252 on lone high bytes. */
            if ((b & 0xE0) == 0xC0 && i + 1 < in_len && (in[i + 1] & 0xC0) == 0x80) {
                uint32_t cp = ((uint32_t)(b & 0x1F) << 6) | (in[i + 1] & 0x3F);
                if (cp >= 0x80) {
                    int ch = codepoint_to_latin1(cp);
                    if (ch)
                        out[o++] = (char)ch;
                    i += 2;
                    continue;
                }
            } else if ((b & 0xF0) == 0xE0 && i + 2 < in_len &&
                       (in[i + 1] & 0xC0) == 0x80 && (in[i + 2] & 0xC0) == 0x80) {
                uint32_t cp = ((uint32_t)(b & 0x0F) << 12) |
                              ((uint32_t)(in[i + 1] & 0x3F) << 6) | (in[i + 2] & 0x3F);
                if (cp >= 0x800) {
                    int ch = codepoint_to_latin1(cp);
                    if (ch)
                        out[o++] = (char)ch;
                    i += 3;
                    continue;
                }
            } else if ((b & 0xF8) == 0xF0 && i + 3 < in_len &&
                       (in[i + 1] & 0xC0) == 0x80 && (in[i + 2] & 0xC0) == 0x80 && (in[i + 3] & 0xC0) == 0x80) {
                uint32_t cp = ((uint32_t)(b & 0x07) << 18) | ((uint32_t)(in[i + 1] & 0x3F) << 12) |
                              ((uint32_t)(in[i + 2] & 0x3F) << 6) | (in[i + 3] & 0x3F);
                int ch = codepoint_to_latin1(cp);
                if (ch)
                    out[o++] = (char)ch;
                i += 4;
                continue;
            }
        }
        int ch = codepoint_to_latin1(cp1252_to_unicode(b));
        if (ch)
            out[o++] = (char)ch;
        i++;
    }
    out[o] = 0;
}

/* ---- RFC 2047 encoded-word header decoding ------------------------------ */

static size_t parse_encoded_word(const char *s, char *charset, size_t cs_cap,
                                 char *enc, const char **payload, size_t *pay_len)
{
    if (s[0] != '=' || s[1] != '?')
        return 0;
    const char *q1 = strchr(s + 2, '?');
    if (!q1 || q1 == s + 2 || (size_t)(q1 - (s + 2)) >= cs_cap)
        return 0;
    if ((q1[1] != 'B' && q1[1] != 'b' && q1[1] != 'Q' && q1[1] != 'q') || q1[2] != '?')
        return 0;
    const char *start = q1 + 3;
    const char *end = strstr(start, "?=");
    if (!end)
        return 0;
    for (const char *p = start; p < end; p++)
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            return 0;
    size_t cslen = (size_t)(q1 - (s + 2));
    memcpy(charset, s + 2, cslen);
    charset[cslen] = 0;
    *enc = (char)toupper((unsigned char)q1[1]);
    *payload = start;
    *pay_len = (size_t)(end - start);
    return (size_t)((end + 2) - s);
}

void mime_decode_header(char *out, size_t out_cap, const char *in)
{
    size_t o = 0;
    bool prev_encoded = false;

    if (!out_cap)
        return;
    out[0] = 0;
    if (!in)
        return;
    while (*in == ' ' || *in == '\t' || *in == '\r' || *in == '\n')
        in++;

    for (const char *p = in; *p && o + 1 < out_cap;) {
        /* Collapse CRLF/LF folding and runs of whitespace into a single space. */
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            const char *ws = p;
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                p++;
            if (!*p)
                break;
            char cs[32], enc;
            const char *pay;
            size_t plen;
            if (prev_encoded && parse_encoded_word(p, cs, sizeof(cs), &enc, &pay, &plen) > 0)
                continue; /* RFC 2047: ignore linear whitespace between encoded-words */
            (void)ws;
            if (o > 0 && out[o - 1] != ' ')
                out[o++] = ' ';
            prev_encoded = false;
            continue;
        }

        char cs[32], enc;
        const char *pay;
        size_t plen;
        size_t consumed = parse_encoded_word(p, cs, sizeof(cs), &enc, &pay, &plen);
        if (consumed > 0) {
            uint8_t raw[256];
            char conv[256];
            size_t rlen = 0;
            if (enc == 'B')
                rlen = mime_base64_decode(raw, sizeof(raw), pay, plen);
            else
                rlen = mime_qp_decode(raw, sizeof(raw), pay, plen, true);
            mime_to_latin1(conv, sizeof(conv), raw, rlen, cs);
            for (const char *c = conv; *c && o + 1 < out_cap; c++)
                out[o++] = (*c == '\n' || *c == '\t') ? ' ' : *c;
            p += consumed;
            prev_encoded = true;
            continue;
        }

        /* Plain run up to next whitespace or encoded-word. */
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && !(p[0] == '=' && p[1] == '?'))
            p++;
        char conv[256];
        mime_to_latin1(conv, sizeof(conv), (const uint8_t *)start, (size_t)(p - start), "utf-8");
        for (const char *c = conv; *c && o + 1 < out_cap; c++)
            out[o++] = *c;
        prev_encoded = false;
    }
    while (o > 0 && out[o - 1] == ' ')
        o--;
    out[o] = 0;
}

/* ---- HTML to plain Latin-1 text ----------------------------------------- */

static const struct {
    const char *name;
    uint16_t cp;
} html_entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
    { "nbsp", ' ' }, { "copy", 0xA9 }, { "reg", 0xAE }, { "deg", 0x82 },
    { "bull", 0x2022 }, { "middot", 0xB7 }, { "hellip", 0x2026 },
    { "ndash", 0x2013 }, { "mdash", 0x2014 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 },
    { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "laquo", 0xAB }, { "raquo", 0xBB },
    { "eacute", 0xE9 }, { "egrave", 0xE8 }, { "aacute", 0xE1 }, { "oacute", 0xF3 },
    { "ntilde", 0xF1 }, { "uuml", 0xFC }, { "ouml", 0xF6 }, { "auml", 0xE4 },
};

static size_t decode_entity(const char *s, const char *end, int *ch_out)
{
    const char *semi = s + 1;

    while (semi < end && semi - s < 12 && *semi != ';' && *semi != '&' && *semi != '<' && !isspace((unsigned char)*semi))
        semi++;
    if (semi >= end || *semi != ';')
        return 0;
    size_t len = (size_t)(semi - (s + 1));
    if (!len)
        return 0;
    if (s[1] == '#') {
        bool hex = len >= 2 && (s[2] == 'x' || s[2] == 'X');
        uint32_t v = 0;
        const char *p = s + (hex ? 3 : 2);
        if (p >= semi)
            return 0;
        for (; p < semi; p++) {
            int d = hex ? hex_val((unsigned char)*p) : (isdigit((unsigned char)*p) ? *p - '0' : -1);
            if (d < 0 || v > 0x10FFFF)
                return 0;
            v = v * (hex ? 16u : 10u) + (uint32_t)d;
        }
        int ch = codepoint_to_latin1(v);
        *ch_out = ch ? ch : ' ';
        return len + 2;
    }
    for (size_t i = 0; i < sizeof(html_entities) / sizeof(html_entities[0]); i++) {
        if (strlen(html_entities[i].name) == len && !strncasecmp(s + 1, html_entities[i].name, len)) {
            *ch_out = codepoint_to_latin1(html_entities[i].cp);
            return len + 2;
        }
    }
    return 0;
}

static void emit_newline(char *out, size_t *o, size_t cap, int max_consecutive)
{
    while (*o > 0 && out[*o - 1] == ' ')
        (*o)--;
    int count = 0;
    for (size_t k = *o; k > 0 && out[k - 1] == '\n'; k--)
        count++;
    if (count < max_consecutive && *o + 1 < cap)
        out[(*o)++] = '\n';
}

void mime_html_to_text(char *out, size_t out_cap, const char *in, size_t in_len)
{
    if (!out_cap)
        return;
    out[0] = 0;
    if (!in || !in_len)
        return;

    /* First convert UTF-8 / CP1252 bytes into Latin-1 so tags and text share one encoding. */
    size_t conv_cap = in_len + 1;
    char *conv = malloc(conv_cap);
    if (!conv)
        return;
    mime_to_latin1(conv, conv_cap, (const uint8_t *)in, in_len, "utf-8");

    const char *s = conv, *end = conv + strlen(conv);
    size_t o = 0;
    bool pending_space = false;

    while (s < end && o + 1 < out_cap) {
        if (*s == '<') {
            if (s + 4 <= end && !memcmp(s, "<!--", 4)) {
                const char *ce = strstr(s + 4, "-->");
                s = ce ? ce + 3 : end;
                continue;
            }
            const char *gt = memchr(s, '>', (size_t)(end - s));
            if (!gt)
                break;
            const char *t = s + 1;
            while (t < gt && isspace((unsigned char)*t))
                t++;
            bool closing = (t < gt && *t == '/');
            if (closing)
                t++;
            char tag[16];
            size_t ti = 0;
            while (t < gt && isalnum((unsigned char)*t) && ti + 1 < sizeof(tag))
                tag[ti++] = (char)tolower((unsigned char)*t++);
            tag[ti] = 0;

            /* Skip <head>, <style>, <script> blocks completely. */
            if (!closing && (!strcmp(tag, "style") || !strcmp(tag, "script") || !strcmp(tag, "head"))) {
                char close_pat[20];
                snprintf(close_pat, sizeof(close_pat), "</%s", tag);
                const char * p = gt + 1;
                while (p < end) {
                    const char *lt = memchr(p, '<', (size_t)(end - p));
                    if (!lt) {
                        p = end;
                        break;
                    }
                    if ((size_t)(end - lt) >= strlen(close_pat) &&
                        !strncasecmp(lt, close_pat, strlen(close_pat))) {
                        const char *end_gt = memchr(lt, '>', (size_t)(end - lt));
                        p = end_gt ? end_gt + 1 : end;
                        break;
                    }
                    p = lt + 1;
                }
                s = p;
                continue;
            }

            if (!strcmp(tag, "br")) {
                emit_newline(out, &o, out_cap, 2);
                pending_space = false;
            } else if (!strcmp(tag, "p") || !strcmp(tag, "h1") || !strcmp(tag, "h2") ||
                       !strcmp(tag, "h3") || !strcmp(tag, "blockquote")) {
                if (o > 0)
                    emit_newline(out, &o, out_cap, 2);
                if (closing && !strcmp(tag, "p"))
                    emit_newline(out, &o, out_cap, 2);
                pending_space = false;
            } else if (!strcmp(tag, "div") || !strcmp(tag, "tr") || !strcmp(tag, "table") ||
                       !strcmp(tag, "ul") || !strcmp(tag, "ol")) {
                if (o > 0)
                    emit_newline(out, &o, out_cap, 1);
                pending_space = false;
            } else if (!closing && !strcmp(tag, "li")) {
                if (o > 0)
                    emit_newline(out, &o, out_cap, 1);
                if (o + 2 < out_cap) {
                    out[o++] = (char)0x83; /* bullet */
                    out[o++] = ' ';
                }
                pending_space = false;
            } else if (!closing && (!strcmp(tag, "td") || !strcmp(tag, "th"))) {
                if (o > 0 && out[o - 1] != '\n' && out[o - 1] != ' ')
                    pending_space = true;
            } else if (!closing && !strcmp(tag, "hr")) {
                if (o > 0)
                    emit_newline(out, &o, out_cap, 1);
                for (int k = 0; k < 16 && o + 1 < out_cap; k++)
                    out[o++] = '-';
                emit_newline(out, &o, out_cap, 1);
                pending_space = false;
            }
            s = gt + 1;
            continue;
        }

        if (isspace((unsigned char)*s)) {
            if (o > 0 && out[o - 1] != '\n')
                pending_space = true;
            s++;
            continue;
        }

        int ch = (unsigned char)*s;
        size_t adv = 1;
        if (ch == '&') {
            int ent = 0;
            size_t k = decode_entity(s, end, &ent);
            if (k > 0) {
                ch = ent;
                adv = k;
            }
        }
        if (pending_space && o + 2 < out_cap) {
            out[o++] = ' ';
            pending_space = false;
        }
        if (ch && o + 1 < out_cap)
            out[o++] = (char)ch;
        s += adv;
    }

    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\n'))
        o--;
    out[o] = 0;
    free(conv);
}

/* ---- Addresses, Dates, and Subjects ------------------------------------- */

static void trim_inplace(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '"')
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '"'))
        s[--n] = 0;
}

void mime_parse_addr(const char *raw, char *name_out, size_t name_cap, char *addr_out, size_t addr_cap)
{
    char decoded[256];

    if (name_out && name_cap)
        name_out[0] = 0;
    if (addr_out && addr_cap)
        addr_out[0] = 0;
    if (!raw)
        return;

    mime_decode_header(decoded, sizeof(decoded), raw);
    const char *lt = strchr(decoded, '<');
    const char *gt = lt ? strchr(lt + 1, '>') : NULL;

    char addr[MAIL_ADDR_MAX] = { 0 };
    char name[MAIL_NAME_MAX] = { 0 };

    if (lt && gt && gt > lt + 1) {
        size_t alen = (size_t)(gt - (lt + 1));
        if (alen >= sizeof(addr))
            alen = sizeof(addr) - 1;
        memcpy(addr, lt + 1, alen);
        addr[alen] = 0;
        trim_inplace(addr);

        size_t nlen = (size_t)(lt - decoded);
        if (nlen >= sizeof(name))
            nlen = sizeof(name) - 1;
        memcpy(name, decoded, nlen);
        name[nlen] = 0;
        trim_inplace(name);
    } else {
        /* Plain address, optionally followed by (Comment Name). */
        const char *lp = strchr(decoded, '(');
        const char *rp = lp ? strchr(lp + 1, ')') : NULL;
        if (lp && rp && rp > lp + 1) {
            size_t alen = (size_t)(lp - decoded);
            if (alen >= sizeof(addr))
                alen = sizeof(addr) - 1;
            memcpy(addr, decoded, alen);
            addr[alen] = 0;
            trim_inplace(addr);

            size_t nlen = (size_t)(rp - (lp + 1));
            if (nlen >= sizeof(name))
                nlen = sizeof(name) - 1;
            memcpy(name, lp + 1, nlen);
            name[nlen] = 0;
            trim_inplace(name);
        } else {
            strlcpy(addr, decoded, sizeof(addr));
            trim_inplace(addr);
        }
    }

    if (!name[0] && addr[0])
        strlcpy(name, addr, sizeof(name));

    if (name_out && name_cap)
        strlcpy(name_out, name, name_cap);
    if (addr_out && addr_cap)
        strlcpy(addr_out, addr, addr_cap);
}

int mime_extract_addrs(const char *list, char out[][MAIL_ADDR_MAX], int max_addrs)
{
    int count = 0;

    if (!list || max_addrs <= 0)
        return 0;
    const char *p = list;
    while (*p && count < max_addrs) {
        bool in_quote = false, in_angle = false;
        const char *start = p;
        for (; *p; p++) {
            if (*p == '"')
                in_quote = !in_quote;
            else if (!in_quote && *p == '<')
                in_angle = true;
            else if (!in_quote && *p == '>')
                in_angle = false;
            else if (!in_quote && !in_angle && (*p == ',' || *p == ';'))
                break;
        }
        char item[192];
        size_t len = (size_t)(p - start);
        if (len >= sizeof(item))
            len = sizeof(item) - 1;
        memcpy(item, start, len);
        item[len] = 0;
        char addr[MAIL_ADDR_MAX];
        mime_parse_addr(item, NULL, 0, addr, sizeof(addr));
        if (addr[0] && strchr(addr, '@')) {
            strlcpy(out[count++], addr, MAIL_ADDR_MAX);
        }
        if (*p == ',' || *p == ';')
            p++;
    }
    return count;
}

static const char *const month_names[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

void mime_parse_date(const char *raw, char *out, size_t out_cap)
{
    if (!out_cap)
        return;
    out[0] = 0;
    if (!raw || !*raw)
        return;

    /* Look for a day number (1..31) and a 3-letter month name (RFC 5322 or DD-Mon-YYYY). */
    const char *p = raw;
    const char *comma = strchr(p, ',');
    if (comma)
        p = comma + 1;
    while (*p == ' ' || *p == '\t' || *p == '"')
        p++;

    int day = 0;
    int mon = -1;
    while (*p) {
        if (isdigit((unsigned char)*p) && !day) {
            long v = strtol(p, (char **)&p, 10);
            if (v >= 1 && v <= 31)
                day = (int)v;
            continue;
        }
        if (isalpha((unsigned char)p[0]) && isalpha((unsigned char)p[1]) && isalpha((unsigned char)p[2])) {
            for (int m = 0; m < 12; m++) {
                if (!strncasecmp(p, month_names[m], 3)) {
                    mon = m;
                    break;
                }
            }
            if (mon >= 0)
                break;
        }
        p++;
    }
    if (mon >= 0 && day > 0) {
        snprintf(out, out_cap, "%s %d", month_names[mon], day);
        return;
    }
    strlcpy(out, raw, out_cap);
}

void mime_reply_subject(const char *subj, bool forward, char *out, size_t out_cap)
{
    const char *prefix = forward ? "Fwd: " : "Re: ";

    if (!out_cap)
        return;
    if (!subj || !*subj) {
        strlcpy(out, prefix, out_cap);
        return;
    }
    while (*subj == ' ')
        subj++;
    if (!forward && !strncasecmp(subj, "Re:", 3)) {
        subj += 3;
        while (*subj == ' ')
            subj++;
    } else if (forward && (!strncasecmp(subj, "Fwd:", 4) || !strncasecmp(subj, "Fw:", 3))) {
        subj += (subj[2] == ':' ? 3 : 4);
        while (*subj == ' ')
            subj++;
    }
    snprintf(out, out_cap, "%s%s", prefix, subj);
}

/* ---- MIME Header and Multipart Parsing ---------------------------------- */

/* Extracts a header parameter like boundary="..." or charset=... */
static bool extract_param(const char *hdr, const char *param, char *out, size_t out_cap)
{
    size_t plen = strlen(param);

    out[0] = 0;
    for (const char *p = hdr; *p; p++) {
        if ((p == hdr || isspace((unsigned char)p[-1]) || p[-1] == ';') &&
            !strncasecmp(p, param, plen)) {
            p += plen;
            while (*p == ' ' || *p == '\t')
                p++;
            if (*p != '=')
                continue;
            p++;
            while (*p == ' ' || *p == '\t')
                p++;
            size_t o = 0;
            if (*p == '"') {
                p++;
                while (*p && *p != '"' && o + 1 < out_cap) {
                    if (*p == '\\' && p[1])
                        p++;
                    out[o++] = *p++;
                }
            } else {
                while (*p && *p != ';' && !isspace((unsigned char)*p) && o + 1 < out_cap)
                    out[o++] = *p++;
            }
            out[o] = 0;
            return o > 0;
        }
    }
    return false;
}

/* Splits header block from body; returns offset of body start. */
static size_t find_body_offset(const char *raw, size_t len, size_t *hdr_end)
{
    for (size_t i = 0; i < len; i++) {
        if (raw[i] == '\r' && i + 3 < len && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') {
            *hdr_end = i + 2;
            return i + 4;
        }
        if (raw[i] == '\n' && i + 1 < len && raw[i + 1] == '\n') {
            *hdr_end = i + 1;
            return i + 2;
        }
    }
    *hdr_end = len;
    return len;
}

/* Iterates unfolded RFC 5322 headers in raw[0..hdr_len). */
static size_t next_header(const char *raw, size_t hdr_len, size_t pos,
                          char *name, size_t name_cap, char *val, size_t val_cap)
{
    name[0] = 0;
    val[0] = 0;
    if (pos >= hdr_len)
        return hdr_len;

    /* Find the end of this header, including folded continuation lines (starting with ' ' or '\t'). */
    size_t start = pos;
    size_t cur = pos;
    while (cur < hdr_len) {
        const char *nl = memchr(raw + cur, '\n', hdr_len - cur);
        if (!nl) {
            cur = hdr_len;
            break;
        }
        cur = (size_t)(nl - raw) + 1;
        if (cur >= hdr_len || (raw[cur] != ' ' && raw[cur] != '\t'))
            break;
    }

    const char *colon = memchr(raw + start, ':', cur - start);
    if (!colon)
        return cur;

    size_t nlen = (size_t)(colon - (raw + start));
    while (nlen > 0 && (raw[start + nlen - 1] == ' ' || raw[start + nlen - 1] == '\t'))
        nlen--;
    if (nlen >= name_cap)
        nlen = name_cap - 1;
    memcpy(name, raw + start, nlen);
    name[nlen] = 0;

    size_t vstart = (size_t)(colon - raw) + 1;
    while (vstart < cur && (raw[vstart] == ' ' || raw[vstart] == '\t'))
        vstart++;
    size_t vo = 0;
    for (size_t i = vstart; i < cur && vo + 1 < val_cap; i++) {
        char c = raw[i];
        if (c == '\r' || c == '\n') {
            if (vo > 0 && val[vo - 1] != ' ')
                val[vo++] = ' ';
        } else if (c == '\t') {
            if (vo > 0 && val[vo - 1] != ' ')
                val[vo++] = ' ';
        } else {
            val[vo++] = c;
        }
    }
    while (vo > 0 && val[vo - 1] == ' ')
        vo--;
    val[vo] = 0;
    return cur;
}

void mime_parse_headers(const char *raw, size_t len, struct mail_summary *sum,
                        char *reply_to, size_t rt_cap, char *refs, size_t refs_cap)
{
    size_t hdr_end = len;
    find_body_offset(raw, len, &hdr_end);

    if (reply_to && rt_cap)
        reply_to[0] = 0;
    if (refs && refs_cap)
        refs[0] = 0;

    char name[64], val[512];
    for (size_t pos = 0; pos < hdr_end;) {
        pos = next_header(raw, hdr_end, pos, name, sizeof(name), val, sizeof(val));
        if (!name[0])
            continue;
        if (!strcasecmp(name, "From")) {
            mime_parse_addr(val, sum->from_name, sizeof(sum->from_name), sum->from_addr, sizeof(sum->from_addr));
        } else if (!strcasecmp(name, "To")) {
            mime_decode_header(sum->to, sizeof(sum->to), val);
        } else if (!strcasecmp(name, "Cc")) {
            mime_decode_header(sum->cc, sizeof(sum->cc), val);
        } else if (!strcasecmp(name, "Subject")) {
            mime_decode_header(sum->subject, sizeof(sum->subject), val);
        } else if (!strcasecmp(name, "Date")) {
            mime_parse_date(val, sum->date, sizeof(sum->date));
        } else if (!strcasecmp(name, "Message-ID") || !strcasecmp(name, "Message-Id")) {
            strlcpy(sum->msgid, val, sizeof(sum->msgid));
            trim_inplace(sum->msgid);
        } else if (!strcasecmp(name, "Reply-To") && reply_to && rt_cap) {
            mime_parse_addr(val, NULL, 0, reply_to, rt_cap);
        } else if (!strcasecmp(name, "References") && refs && refs_cap) {
            strlcpy(refs, val, refs_cap);
            trim_inplace(refs);
        }
    }
    if (!sum->subject[0])
        strlcpy(sum->subject, "(No Subject)", sizeof(sum->subject));
    if (!sum->from_name[0] && sum->from_addr[0])
        strlcpy(sum->from_name, sum->from_addr, sizeof(sum->from_name));
}

static void make_snippet(const char *body, char *snippet, size_t cap)
{
    size_t o = 0;
    bool sp = false;

    if (!cap)
        return;
    for (const char *p = body; *p && o + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            if (o > 0)
                sp = true;
            continue;
        }
        /* Skip leading '>' quote markers in snippet when possible. */
        if (o == 0 && c == '>')
            continue;
        if (sp && o + 2 < cap) {
            snippet[o++] = ' ';
            sp = false;
        }
        snippet[o++] = (char)c;
    }
    snippet[o] = 0;
}

struct parse_state {
    char plain[MAIL_BODY_MAX];
    char html[MAIL_BODY_MAX];
    bool has_plain, has_html;
    struct mail_att *atts;
    int *natts;
    uint8_t *flags;
};

static void append_body(char *dst, size_t cap, const char *add)
{
    if (!add || !*add)
        return;
    size_t cur = strlen(dst);
    if (cur > 0 && cur + 2 < cap) {
        dst[cur++] = '\n';
        dst[cur++] = '\n';
        dst[cur] = 0;
    }
    strlcat(dst, add, cap);
}

static void parse_entity(const char *raw, size_t len, struct parse_state *st, int depth);

static void parse_multipart(const char *body, size_t len, const char *boundary,
                            bool is_alternative, struct parse_state *st, int depth)
{
    size_t blen = strlen(boundary);
    if (!blen || blen > 120 || depth > 5)
        return;

    struct parse_state alt_st;
    struct parse_state *target = st;
    if (is_alternative) {
        memset(&alt_st, 0, sizeof(alt_st));
        alt_st.atts = st->atts;
        alt_st.natts = st->natts;
        alt_st.flags = st->flags;
        target = &alt_st;
    }

    const char *p = body, *end = body + len;
    const char *part_start = NULL;

    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        const char *next_line = line_end ? line_end + 1 : end;
        size_t l = (size_t)((line_end ? line_end : end) - p);
        if (l > 0 && p[l - 1] == '\r')
            l--;

        if (l >= 2 + blen && p[0] == '-' && p[1] == '-' && !memcmp(p + 2, boundary, blen)) {
            bool is_final = (l >= 4 + blen && p[2 + blen] == '-' && p[3 + blen] == '-');
            const char *rest = p + 2 + blen + (is_final ? 2 : 0);
            while (rest < p + l && (*rest == ' ' || *rest == '\t'))
                rest++;
            if (rest == p + l) {
                if (part_start && p > part_start) {
                    size_t plen = (size_t)(p - part_start);
                    if (plen > 0 && part_start[plen - 1] == '\n')
                        plen--;
                    if (plen > 0 && part_start[plen - 1] == '\r')
                        plen--;
                    parse_entity(part_start, plen, target, depth + 1);
                }
                if (is_final)
                    break;
                part_start = next_line;
            }
        }
        p = next_line;
    }

    if (is_alternative) {
        if (alt_st.has_plain && alt_st.plain[0]) {
            append_body(st->plain, sizeof(st->plain), alt_st.plain);
            st->has_plain = true;
        } else if (alt_st.has_html && alt_st.html[0]) {
            append_body(st->plain, sizeof(st->plain), alt_st.html);
            st->has_plain = true;
        }
    }
}

static void parse_entity(const char *raw, size_t len, struct parse_state *st, int depth)
{
    if (depth > 5 || !len)
        return;

    size_t hdr_end = 0;
    size_t body_off = find_body_offset(raw, len, &hdr_end);
    const char *body = raw + body_off;
    size_t body_len = len - body_off;

    char ctype[256] = "text/plain";
    char cte[64] = "7bit";
    char cdisp[256] = "";

    char hname[64], hval[512];
    for (size_t pos = 0; pos < hdr_end;) {
        pos = next_header(raw, hdr_end, pos, hname, sizeof(hname), hval, sizeof(hval));
        if (!strcasecmp(hname, "Content-Type"))
            strlcpy(ctype, hval, sizeof(ctype));
        else if (!strcasecmp(hname, "Content-Transfer-Encoding"))
            strlcpy(cte, hval, sizeof(cte));
        else if (!strcasecmp(hname, "Content-Disposition"))
            strlcpy(cdisp, hval, sizeof(cdisp));
    }

    /* Extract primary media type/subtype. */
    char mtype[64];
    size_t mi = 0;
    for (const char *p = ctype; *p && *p != ';' && !isspace((unsigned char)*p) && mi + 1 < sizeof(mtype); p++)
        mtype[mi++] = (char)tolower((unsigned char)*p);
    mtype[mi] = 0;

    if (!strncasecmp(mtype, "multipart/", 10)) {
        char boundary[128];
        if (extract_param(ctype, "boundary", boundary, sizeof(boundary))) {
            bool is_alt = !strcasecmp(mtype, "multipart/alternative");
            parse_multipart(body, body_len, boundary, is_alt, st, depth);
        }
        return;
    }

    /* Check if this part is an attachment. */
    char filename[64] = "";
    if (!extract_param(cdisp, "filename", filename, sizeof(filename)))
        extract_param(ctype, "name", filename, sizeof(filename));
    bool is_att = (!strncasecmp(cdisp, "attachment", 10)) ||
                  (filename[0] && strcasecmp(mtype, "text/plain") && strcasecmp(mtype, "text/html"));

    if (is_att || (strcasecmp(mtype, "text/plain") && strcasecmp(mtype, "text/html") && mtype[0])) {
        if (filename[0] || !strncasecmp(cdisp, "attachment", 10)) {
            if (st->flags)
                *st->flags |= MAIL_F_HAS_ATT;
            if (st->atts && st->natts && *st->natts < MAIL_MAX_ATTS) {
                struct mail_att *a = &st->atts[(*st->natts)++];
                if (filename[0])
                    mime_decode_header(a->name, sizeof(a->name), filename);
                else
                    strlcpy(a->name, "attachment", sizeof(a->name));
                strlcpy(a->type, mtype[0] ? mtype : "application/octet-stream", sizeof(a->type));
                a->size = !strncasecmp(cte, "base64", 6) ? (uint32_t)(body_len * 3 / 4) : (uint32_t)body_len;
            }
        }
        return;
    }

    /* Decode transfer encoding of text/plain or text/html part. */
    size_t dec_cap = body_len + 1;
    if (dec_cap > MAIL_FETCH_MAX)
        dec_cap = MAIL_FETCH_MAX;
    uint8_t *dec = malloc(dec_cap);
    if (!dec)
        return;
    size_t dec_len = 0;
    if (!strncasecmp(cte, "base64", 6)) {
        dec_len = mime_base64_decode(dec, dec_cap - 1, body, body_len);
    } else if (!strncasecmp(cte, "quoted-printable", 16)) {
        dec_len = mime_qp_decode(dec, dec_cap - 1, body, body_len, false);
    } else {
        dec_len = body_len < dec_cap - 1 ? body_len : dec_cap - 1;
        memcpy(dec, body, dec_len);
    }
    dec[dec_len] = 0;

    char charset[32] = "utf-8";
    extract_param(ctype, "charset", charset, sizeof(charset));

    char *text = malloc(MAIL_BODY_MAX);
    if (text) {
        if (!strcasecmp(mtype, "text/html")) {
            char *latin = malloc(dec_len + 1);
            if (latin) {
                mime_to_latin1(latin, dec_len + 1, dec, dec_len, charset);
                mime_html_to_text(text, MAIL_BODY_MAX, latin, strlen(latin));
                free(latin);
            } else {
                mime_html_to_text(text, MAIL_BODY_MAX, (const char *)dec, dec_len);
            }
            if (text[0]) {
                append_body(st->html, sizeof(st->html), text);
                st->has_html = true;
            }
        } else {
            mime_to_latin1(text, MAIL_BODY_MAX, dec, dec_len, charset);
            /* Trim trailing whitespace/newlines. */
            size_t tlen = strlen(text);
            while (tlen > 0 && (text[tlen - 1] == ' ' || text[tlen - 1] == '\n'))
                text[--tlen] = 0;
            if (text[0]) {
                append_body(st->plain, sizeof(st->plain), text);
                st->has_plain = true;
            }
        }
        free(text);
    }
    free(dec);
}

void mime_parse_message(const char *raw, size_t len, struct mail_message *msg)
{
    uint32_t saved_uid = msg->hdr.uid;
    uint8_t saved_flags = msg->hdr.flags;
    char saved_date[MAIL_DATE_MAX];
    strlcpy(saved_date, msg->hdr.date, sizeof(saved_date));

    memset(msg, 0, sizeof(*msg));
    msg->hdr.uid = saved_uid;
    msg->hdr.flags = saved_flags;

    mime_parse_headers(raw, len, &msg->hdr, msg->reply_to, sizeof(msg->reply_to),
                       msg->references, sizeof(msg->references));
    if (!msg->hdr.date[0] && saved_date[0])
        strlcpy(msg->hdr.date, saved_date, sizeof(msg->hdr.date));

    struct parse_state *st = calloc(1, sizeof(*st));
    if (!st)
        return;
    st->atts = msg->atts;
    st->natts = &msg->natts;
    st->flags = &msg->hdr.flags;

    parse_entity(raw, len, st, 0);
    if (st->has_plain && st->plain[0])
        strlcpy(msg->body, st->plain, sizeof(msg->body));
    else if (st->has_html && st->html[0])
        strlcpy(msg->body, st->html, sizeof(msg->body));

    make_snippet(msg->body, msg->hdr.snippet, sizeof(msg->hdr.snippet));
    free(st);
}

/* ---- Building RFC 5322 Messages and SASL Tokens ------------------------- */

size_t mime_build_message(char *out, size_t out_cap, const char *from_name, const char *from_addr,
                          const char *to, const char *cc, const char *subject,
                          const char *in_reply_to, const char *references,
                          const char *date_rfc, const char *msgid, const char *body)
{
    size_t o = 0;

    if (!out_cap)
        return 0;
    out[0] = 0;

#define APPEND_FMT(...) do { \
    if (o < out_cap) { \
        int _n = snprintf(out + o, out_cap - o, __VA_ARGS__); \
        if (_n > 0) \
            o += (size_t)_n < out_cap - o ? (size_t)_n : out_cap - o - 1; \
    } \
} while (0)

    if (from_name && *from_name && strcmp(from_name, from_addr))
        APPEND_FMT("From: \"%s\" <%s>\r\n", from_name, from_addr);
    else
        APPEND_FMT("From: <%s>\r\n", from_addr ? from_addr : "");

    APPEND_FMT("To: %s\r\n", to ? to : "");
    if (cc && *cc)
        APPEND_FMT("Cc: %s\r\n", cc);
    APPEND_FMT("Subject: %s\r\n", (subject && *subject) ? subject : "(No Subject)");
    if (date_rfc && *date_rfc)
        APPEND_FMT("Date: %s\r\n", date_rfc);
    if (msgid && *msgid)
        APPEND_FMT("Message-ID: %s\r\n", msgid);
    if (in_reply_to && *in_reply_to)
        APPEND_FMT("In-Reply-To: %s\r\n", in_reply_to);
    if (references && *references)
        APPEND_FMT("References: %s\r\n", references);
    else if (in_reply_to && *in_reply_to)
        APPEND_FMT("References: %s\r\n", in_reply_to);

    APPEND_FMT("MIME-Version: 1.0\r\n");
    APPEND_FMT("Content-Type: text/plain; charset=ISO-8859-1\r\n");
    APPEND_FMT("Content-Transfer-Encoding: 8bit\r\n");
    APPEND_FMT("User-Agent: Glaze-Gmail/1.0\r\n\r\n");

    if (body) {
        for (const char *p = body; *p && o + 3 < out_cap; p++) {
            if (*p == '\r')
                continue;
            if (*p == '\n') {
                out[o++] = '\r';
                out[o++] = '\n';
            } else {
                out[o++] = *p;
            }
        }
    }
    if (o >= 2 && !(out[o - 2] == '\r' && out[o - 1] == '\n') && o + 3 < out_cap) {
        out[o++] = '\r';
        out[o++] = '\n';
    }
    out[o] = 0;
#undef APPEND_FMT
    return o;
}

size_t mime_sasl_plain(char *out, size_t out_cap, const char *user, const char *pass)
{
    uint8_t raw[MAIL_USER_MAX + MAIL_PASS_MAX + 4];
    size_t ulen = user ? strlen(user) : 0;
    size_t plen = pass ? strlen(pass) : 0;

    if (1 + ulen + 1 + plen > sizeof(raw))
        return 0;
    raw[0] = 0;
    memcpy(raw + 1, user, ulen);
    raw[1 + ulen] = 0;
    memcpy(raw + 2 + ulen, pass, plen);
    return mime_base64_encode(out, out_cap, raw, 2 + ulen + plen);
}
