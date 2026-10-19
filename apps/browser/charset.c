/*
 * Character sets: documents are decoded from UTF-8 or Windows-1252 into
 * engine text (see web.h). Code points the fonts lack map onto the extra
 * glyphs, onto ASCII look-alikes, or onto '?'.
 */

#include "web.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Windows-1252 0x80-0x9F (0 = undefined). */
static const uint16_t cp1252[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

/* Base letters for U+0100-U+024F (from Unicode decompositions). */
static const char latin_ext[] =
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOoOo??RrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs"
    "bBBb???Cc?DDd????FfG???IKkl??NnOOo??Pp?????tTtTUu?VYyZz?????????"
    "????DDdLLlNNnAaIiOoUuUuUuUuUu?AaAa??GgGgKkOoOo??jDDdGg??NnAa??Oo"
    "AaAaEeEeIiIiOoOoRrRrUuUuSsTt??HhNd??ZzAaEeOoOoOoOoYylnt???ACcLTs"
    "z??BU?EeJj?qRrYy";

static int put_str(char *out, const char *s)
{
    int n = (int)strlen(s);

    memcpy(out, s, (size_t)n);
    return n;
}

int cs_put(uint32_t cp, char *out)
{
    if (cp < 0x20) {
        if (cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f') {
            out[0] = (char)cp;
            return 1;
        }
        return 0;
    }
    if (cp < 0x7F) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0xA0)
        return 0;
    if (cp == 0xAD)
        return 0;                   /* soft hyphen */
    if (cp <= 0xFF) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp == 0x152)
        return put_str(out, "OE");
    if (cp == 0x153)
        return put_str(out, "oe");
    if (cp >= 0x100 && cp < 0x100 + sizeof(latin_ext) - 1) {
        out[0] = latin_ext[cp - 0x100];
        return 1;
    }
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: case 0x02BC:
        out[0] = '\'';
        return 1;
    case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033:
        out[0] = '"';
        return 1;
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2043:
        out[0] = '-';
        return 1;
    case 0x2026:
        return put_str(out, GFX_CH_ELLIPSIS);
    case 0x2022: case 0x2023: case 0x2219: case 0x25CF: case 0x25AA: case 0x25A0: case 0x30FB:
        return put_str(out, GFX_CH_BULLET);
    case 0x2212:
        return put_str(out, GFX_CH_MINUS);
    case 0x2039: case 0x2190: case 0x25C0: case 0x25C4: case 0x2B05:
        return put_str(out, GFX_CH_LARROW);
    case 0x203A: case 0x2192: case 0x25B6: case 0x25BA: case 0x27A1: case 0x2794:
        return put_str(out, GFX_CH_RARROW);
    case 0x2713: case 0x2714: case 0x2705:
        return put_str(out, GFX_CH_CHECK);
    case 0x2122:
        return put_str(out, "TM");
    case 0x20AC:
        return put_str(out, "EUR");
    case 0x2020:
        out[0] = '+';
        return 1;
    case 0x2264:
        return put_str(out, "<=");
    case 0x2265:
        return put_str(out, ">=");
    case 0x2260:
        return put_str(out, "!=");
    case 0x02C6:
        out[0] = '^';
        return 1;
    case 0x02DC: case 0x223C:
        out[0] = '~';
        return 1;
    case 0x2044: case 0x2215:
        out[0] = '/';
        return 1;
    case 0x2217:
        out[0] = '*';
        return 1;
    case 0x25E6: case 0x25CB:
        out[0] = 'o';
        return 1;
    case 0x2002: case 0x2003: case 0x2004: case 0x2005: case 0x2006: case 0x2007: case 0x2008:
    case 0x2009: case 0x200A: case 0x205F: case 0x3000:
        out[0] = ' ';
        return 1;
    case 0x202F:
        out[0] = (char)0xA0;
        return 1;
    case 0x200B: case 0x200C: case 0x200D: case 0x200E: case 0x200F: case 0x2060: case 0xFEFF:
        return 0;
    }
    /* Variation selectors, emoji and symbol blocks: nothing to show. */
    if ((cp >= 0xFE00 && cp <= 0xFE0F) || cp >= 0x1F000 || (cp >= 0x2600 && cp <= 0x27BF) ||
        (cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0x2B00 && cp <= 0x2BFF))
        return 0;
    out[0] = '?';
    return 1;
}

int cs_from_label(const char *label, int len)
{
    static const char *const utf8[] = { "utf-8", "utf8", "unicode-1-1-utf-8" };
    static const char *const w1252[] = {
        "iso-8859-1", "iso8859-1", "latin1", "l1", "windows-1252", "cp1252", "us-ascii", "ascii",
        "iso-8859-15", "iso_8859-1", "x-cp1252", "ansi_x3.4-1968",
    };

    while (len && (*label == ' ' || *label == '"' || *label == '\'')) {
        label++;
        len--;
    }
    while (len && (label[len - 1] == ' ' || label[len - 1] == '"' || label[len - 1] == '\''))
        len--;
    for (size_t i = 0; i < sizeof(utf8) / sizeof(utf8[0]); i++)
        if ((int)strlen(utf8[i]) == len && !strncasecmp(label, utf8[i], (size_t)len))
            return CS_UTF8;
    for (size_t i = 0; i < sizeof(w1252) / sizeof(w1252[0]); i++)
        if ((int)strlen(w1252[i]) == len && !strncasecmp(label, w1252[i], (size_t)len))
            return CS_1252;
    return -1;
}

/* The value of "charset=" in s (up to end), or -1. */
static int charset_param(const char *s, const char *end)
{
    for (const char *p = s; p + 8 <= end; p++) {
        if (strncasecmp(p, "charset=", 8))
            continue;
        const char *v = p + 8, *e = v;
        while (e < end && *e != ';' && *e != '>' && *e != ' ' && *e != '/' && *e != '\n')
            e++;
        if (*v == '"' || *v == '\'') {
            char q = *v++;
            e = v;
            while (e < end && *e != q)
                e++;
        }
        return cs_from_label(v, (int)(e - v));
    }
    return -1;
}

/* Valid UTF-8 with at least one multibyte sequence (over the first 8 KiB). */
static int utf8_likely(const uint8_t *d, int len)
{
    bool multi = false;

    if (len > 8192)
        len = 8192;
    for (int i = 0; i < len;) {
        uint8_t c = d[i];
        int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0 || (n == 1 && c < 0xC2))
            return 0;
        if (i + n >= len)
            break;                  /* cut at the sample boundary */
        for (int k = 1; k <= n; k++)
            if ((d[i + k] & 0xC0) != 0x80)
                return 0;
        multi |= n > 0;
        i += n + 1;
    }
    return multi ? 1 : -1;
}

int cs_sniff(const char *content_type, const uint8_t *data, int len)
{
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
        return CS_UTF8;
    if (content_type) {
        int cs = charset_param(content_type, content_type + strlen(content_type));
        if (cs >= 0)
            return cs;
    }
    /* <meta charset=...> or <meta http-equiv content="...; charset=...">. */
    const char *s = (const char *)data;
    int scan = len < 2048 ? len : 2048;
    for (int i = 0; i + 5 < scan; i++) {
        if (s[i] != '<' || strncasecmp(s + i + 1, "meta", 4))
            continue;
        const char *gt = memchr(s + i, '>', (size_t)(scan - i));
        if (!gt)
            break;
        int cs = charset_param(s + i, gt);
        if (cs >= 0)
            return cs;
    }
    int u = utf8_likely(data, len);
    return u == 0 ? CS_1252 : CS_UTF8;
}

char *cs_decode(int cs, const uint8_t *in, int len, int *out_len)
{
    /* Each input byte yields at most 3 output bytes ("EUR"), and a UTF-8
     * sequence of 2+ bytes at most 3. */
    char *out = malloc((size_t)len * 3 + 1);
    int o = 0;

    if (!out)
        return NULL;
    int i = 0;
    if (cs == CS_UTF8 && len >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF)
        i = 3;
    while (i < len) {
        uint32_t c = in[i];
        if (cs == CS_1252) {
            i++;
            if (c >= 0x80 && c < 0xA0)
                c = cp1252[c - 0x80] ? cp1252[c - 0x80] : '?';
        } else if (c < 0x80) {
            i++;
        } else {
            int n = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
            uint32_t cp = n == 1 ? c & 0x1F : n == 2 ? c & 0x0F : c & 0x07;
            int k = 1;
            for (; k <= n && i + k < len && (in[i + k] & 0xC0) == 0x80; k++)
                cp = cp << 6 | (in[i + k] & 0x3F);
            if (!n || k <= n || (n == 1 && cp < 0x80) || (n == 2 && cp < 0x800)) {
                c = 0xFFFD;
                i += k > 1 ? k : 1;     /* skip the broken sequence */
            } else {
                c = cp;
                i += n + 1;
            }
        }
        if (c == 0)
            continue;
        o += cs_put(c, out + o);
    }
    out[o] = 0;
    if (out_len)
        *out_len = o;
    return out;
}
