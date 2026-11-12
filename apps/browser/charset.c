/*
 * Character sets: documents are decoded from UTF-8, Windows-1252 or one of
 * the single-byte Central European, Cyrillic, Greek and Turkish sets into
 * engine text (see web.h). Code points the fonts lack map onto the extra
 * glyphs, onto ASCII look-alikes, or onto '?'. Cyrillic and Greek, which the
 * Latin-only fonts can't draw, are written out in Latin letters.
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

/* Windows-1250 0x80-0xBF (0 = undefined). */
static const uint16_t cp1250_lo[64] = {
    0x20AC, 0, 0x201A, 0, 0x201E, 0x2026, 0x2020, 0x2021,
    0, 0x2030, 0x0160, 0x2039, 0x015A, 0x0164, 0x017D, 0x0179,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0, 0x2122, 0x0161, 0x203A, 0x015B, 0x0165, 0x017E, 0x017A,
    0x00A0, 0x02C7, 0x02D8, 0x0141, 0x00A4, 0x0104, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x015E, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x017B,
    0x00B0, 0x00B1, 0x02DB, 0x0142, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
    0x00B8, 0x0105, 0x015F, 0x00BB, 0x013D, 0x02DD, 0x013E, 0x017C,
};

/* ISO-8859-2 0xA0-0xFF; Windows-1250 shares 0xC0-0xFF. */
static const uint16_t latin2_a0[96] = {
    0x00A0, 0x0104, 0x02D8, 0x0141, 0x00A4, 0x013D, 0x015A, 0x00A7,
    0x00A8, 0x0160, 0x015E, 0x0164, 0x0179, 0x00AD, 0x017D, 0x017B,
    0x00B0, 0x0105, 0x02DB, 0x0142, 0x00B4, 0x013E, 0x015B, 0x02C7,
    0x00B8, 0x0161, 0x015F, 0x0165, 0x017A, 0x02DD, 0x017E, 0x017C,
    0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7,
    0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
    0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7,
    0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
    0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7,
    0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
    0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7,
    0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9,
};

/* Windows-1251 0x80-0xBF; 0xC0-0xFF are U+0410-U+044F. */
static const uint16_t cp1251_lo[64] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
};

/* KOI8-R 0xC0-0xDF as offsets from U+0430; 0xE0-0xFF are their capitals. */
static const uint8_t koi8_lower[32] = {
    30, 0, 1, 22, 4, 5, 20, 3, 21, 8, 9, 10, 11, 12, 13, 14,
    15, 31, 16, 17, 18, 19, 6, 2, 28, 27, 7, 24, 29, 25, 23, 26,
};

/*
 * Romanizations, one word per letter ("-" for none). Cyrillic follows U+0430-
 * U+045F (the capitals at U+0400-U+042F are the same letters), roughly BGN/PCGN
 * with the signs dropped; Greek follows U+03B1-U+03C9, roughly ELOT 743.
 */
static const char cyrillic[] =
    "a b v g d e zh z i y k l m n o p r s t u f kh ts ch sh shch - y - e yu ya "
    "e yo dj g ye dz i yi j lj nj c k i u dz";
static const char greek[] = "a v g d e z i th i k l m n x o p r s s t y f ch ps o";

/* The n-th word of a table, capitalized for a capital letter. */
static int put_word(char *out, const char *table, int n, bool upper)
{
    while (n-- > 0) {
        table += strcspn(table, " ");
        if (*table)
            table++;
    }
    int len = (int)strcspn(table, " ");
    if (!len || *table == '-')
        return 0;
    memcpy(out, table, (size_t)len);
    if (upper)
        out[0] = (char)(out[0] - 'a' + 'A');
    return len;
}

/* The unaccented small letter of an accented or capital Greek one. */
static uint32_t greek_small(uint32_t cp, bool *upper)
{
    static const uint8_t tonos[] = {    /* U+0386-U+0390, as offsets from alpha */
        0, 0xFF, 4, 6, 8, 0xFF, 14, 0xFF, 20, 24, 8,
    };
    static const uint8_t tonos2[] = { 8, 20, 0, 4, 6, 8, 20 };  /* U+03AA-U+03B0 */
    static const uint8_t tonos3[] = { 8, 20, 14, 20, 24 };      /* U+03CA-U+03CE */

    *upper = false;
    if (cp >= 0x3B1 && cp <= 0x3C9)
        return cp;
    if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) {
        *upper = true;
        return cp + 0x20;
    }
    if (cp >= 0x386 && cp <= 0x390 && tonos[cp - 0x386] != 0xFF) {
        *upper = cp != 0x390;
        return 0x3B1 + tonos[cp - 0x386];
    }
    if (cp >= 0x3AA && cp <= 0x3B0) {
        *upper = cp < 0x3AC;
        return 0x3B1 + tonos2[cp - 0x3AA];
    }
    if (cp >= 0x3CA && cp <= 0x3CE)
        return 0x3B1 + tonos3[cp - 0x3CA];
    return 0;
}

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
    if (cp >= 0x400 && cp <= 0x45F) {
        bool upper = cp < 0x430;
        return put_word(out, cyrillic, (int)(cp < 0x410 ? cp - 0x400 + 32 : upper ? cp - 0x410 : cp - 0x430), upper);
    }
    if (cp == 0x490 || cp == 0x491) {       /* Ukrainian ghe with upturn */
        out[0] = cp == 0x490 ? 'G' : 'g';
        return 1;
    }
    if (cp >= 0x384 && cp <= 0x3CE) {
        bool upper;
        uint32_t small = greek_small(cp, &upper);
        if (small)
            return put_word(out, greek, (int)(small - 0x3B1), upper);
        if (cp == 0x384 || cp == 0x385)
            return 0;                       /* a tonos by itself */
    }
    switch (cp) {
    case 0x2116:
        return put_str(out, "No");
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
    static const struct {
        const char *name;
        int cs;
    } labels[] = {
        { "utf-8", CS_UTF8 }, { "utf8", CS_UTF8 }, { "unicode-1-1-utf-8", CS_UTF8 },
        { "iso-8859-1", CS_1252 }, { "iso8859-1", CS_1252 }, { "latin1", CS_1252 }, { "l1", CS_1252 },
        { "windows-1252", CS_1252 }, { "cp1252", CS_1252 }, { "us-ascii", CS_1252 }, { "ascii", CS_1252 },
        { "iso-8859-15", CS_1252 }, { "iso_8859-1", CS_1252 }, { "x-cp1252", CS_1252 },
        { "ansi_x3.4-1968", CS_1252 },
        { "windows-1250", CS_1250 }, { "cp1250", CS_1250 }, { "x-cp1250", CS_1250 },
        { "iso-8859-2", CS_LATIN2 }, { "iso8859-2", CS_LATIN2 }, { "iso_8859-2", CS_LATIN2 },
        { "latin2", CS_LATIN2 }, { "l2", CS_LATIN2 },
        { "windows-1251", CS_1251 }, { "cp1251", CS_1251 }, { "x-cp1251", CS_1251 },
        { "koi8-r", CS_KOI8 }, { "koi8-u", CS_KOI8 }, { "koi8", CS_KOI8 }, { "koi8-ru", CS_KOI8 },
        { "iso-8859-5", CS_8859_5 }, { "iso8859-5", CS_8859_5 }, { "iso_8859-5", CS_8859_5 },
        { "cyrillic", CS_8859_5 },
        { "iso-8859-7", CS_8859_7 }, { "iso8859-7", CS_8859_7 }, { "iso_8859-7", CS_8859_7 },
        { "greek", CS_8859_7 }, { "greek8", CS_8859_7 },
        { "windows-1253", CS_1253 }, { "cp1253", CS_1253 }, { "x-cp1253", CS_1253 },
        { "windows-1254", CS_1254 }, { "cp1254", CS_1254 }, { "x-cp1254", CS_1254 },
        { "iso-8859-9", CS_1254 }, { "iso8859-9", CS_1254 }, { "latin5", CS_1254 },
    };

    while (len && (*label == ' ' || *label == '"' || *label == '\'')) {
        label++;
        len--;
    }
    while (len && (label[len - 1] == ' ' || label[len - 1] == '"' || label[len - 1] == '\''))
        len--;
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++)
        if ((int)strlen(labels[i].name) == len && !strncasecmp(label, labels[i].name, (size_t)len))
            return labels[i].cs;
    return -1;
}

/* A byte from 0x80 up in a single-byte set, as a code point; 0 for none. */
static uint32_t byte_cp(int cs, uint32_t c)
{
    switch (cs) {
    case CS_1254:
        switch (c) {
        case 0xD0: return 0x11E;
        case 0xDD: return 0x130;
        case 0xDE: return 0x15E;
        case 0xF0: return 0x11F;
        case 0xFD: return 0x131;
        case 0xFE: return 0x15F;
        }
        break;
    case CS_1250:
        return c < 0xC0 ? cp1250_lo[c - 0x80] : latin2_a0[c - 0xA0];
    case CS_LATIN2:
        return c < 0xA0 ? 0 : latin2_a0[c - 0xA0];
    case CS_1251:
        return c < 0xC0 ? cp1251_lo[c - 0x80] : 0x410 + c - 0xC0;
    case CS_KOI8:
        if (c >= 0xC0)
            return 0x430 + koi8_lower[c & 31] - (c >= 0xE0 ? 0x20 : 0);
        switch (c) {                /* with KOI8-U's letters */
        case 0xA3: return 0x451;
        case 0xB3: return 0x401;
        case 0xA4: return 0x454;
        case 0xA6: return 0x456;
        case 0xA7: return 0x457;
        case 0xAD: return 0x491;
        case 0xB4: return 0x404;
        case 0xB6: return 0x406;
        case 0xB7: return 0x407;
        case 0xBD: return 0x490;
        case 0x9A: return 0xA0;
        case 0xBF: return 0xA9;
        }
        return ' ';                 /* box drawing */
    case CS_8859_5:
        if (c < 0xA0)
            return 0;
        if (c == 0xA0 || c == 0xAD)
            return c;
        if (c == 0xF0)
            return 0x2116;
        if (c == 0xFD)
            return 0xA7;
        return c < 0xB0 ? 0x401 + c - 0xA1 : 0x410 + c - 0xB0;
    case CS_8859_7:
    case CS_1253:
        if (c < 0xA0) {
            if (cs == CS_8859_7)
                return 0;
            break;                  /* as Windows-1252, near enough */
        }
        if (c >= 0xB8 && c != 0xBB && c != 0xBD)
            return c == 0xD2 || c == 0xFF ? 0 : c + 0x2D0;
        if (c == 0xA1 || c == 0xA2)
            return cs == CS_1253 ? 0x385 + c - 0xA1 : 0x2018 + c - 0xA1;
        if (c == 0xB4 || (cs == CS_8859_7 && (c == 0xB5 || c == 0xB6)))
            return 0x384 + c - 0xB4;
        return c == 0xAF ? 0x2015 : c;
    }
    if (c < 0xA0)
        return cp1252[c - 0x80] ? cp1252[c - 0x80] : '?';
    return c;
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
     * sequence of 2+ bytes at most 4; a Cyrillic byte can be 4 ("shch"). */
    bool cyr = cs == CS_1251 || cs == CS_KOI8 || cs == CS_8859_5;
    char *out = malloc((size_t)len * (cyr ? 4 : 3) + 1);
    int o = 0;

    if (!out)
        return NULL;
    int i = 0;
    if (cs == CS_UTF8 && len >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF)
        i = 3;
    while (i < len) {
        uint32_t c = in[i];
        if (cs != CS_UTF8) {
            i++;
            if (c >= 0x80)
                c = byte_cp(cs, c);
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
    /* The page keeps this text; give back what the worst case didn't need. */
    char *fit = realloc(out, (size_t)o + 1);
    return fit ? fit : out;
}
