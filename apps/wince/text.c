/* Code page conversions and wsprintfW formatting, independent of the runtime
 * so host tests can check them. */
#include "text.h"

/* Windows-1252 0x80-0x9F; the five undefined bytes map to themselves, as
 * MultiByteToWideChar does, so documents round-trip. */
static const uint16_t cp1252_high[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008d, 0x017d, 0x008f, 0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
    0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178};

uint16_t ce_cp1252_decode(uint8_t c)
{
    return c >= 0x80 && c < 0xa0 ? cp1252_high[c - 0x80] : c;
}

int ce_cp1252_encode(uint16_t c)
{
    if (c < 0x80 || (c >= 0xa0 && c < 0x100))
        return c;
    for (unsigned i = 0; i < 32; i++)
        if (cp1252_high[i] == c)
            return (int)(0x80 + i);
    return -1;
}

unsigned ce_utf8_decode(const uint8_t *in, size_t length, uint16_t *out, unsigned capacity)
{
    unsigned n = 0;
    for (size_t i = 0; i < length;) {
        uint32_t c = in[i++];
        if (c >= 0xc0 && c < 0xe0 && i < length && (in[i] & 0xc0) == 0x80) {
            c = (c & 31) << 6 | (in[i++] & 63);
        } else if (c >= 0xe0 && c < 0xf0 && i + 1 < length && (in[i] & 0xc0) == 0x80 &&
                   (in[i + 1] & 0xc0) == 0x80) {
            c = (c & 15) << 12 | (uint32_t)(in[i] & 63) << 6 | (in[i + 1] & 63);
            i += 2;
        } else if (c >= 128)
            c = 0xfffd;
        if (out && n < capacity)
            out[n] = (uint16_t)c;
        n++;
    }
    return n;
}

unsigned ce_utf8_encode(const uint16_t *in, size_t length, uint8_t *out, unsigned capacity)
{
    unsigned n = 0;
    for (size_t i = 0; i < length; i++) {
        uint32_t c = in[i];
        uint8_t bytes[3];
        unsigned count;
        if (c < 0x80) {
            bytes[0] = (uint8_t)c;
            count = 1;
        } else if (c < 0x800) {
            bytes[0] = (uint8_t)(0xc0 | c >> 6);
            bytes[1] = (uint8_t)(0x80 | (c & 63));
            count = 2;
        } else {
            bytes[0] = (uint8_t)(0xe0 | c >> 12);
            bytes[1] = (uint8_t)(0x80 | ((c >> 6) & 63));
            bytes[2] = (uint8_t)(0x80 | (c & 63));
            count = 3;
        }
        for (unsigned j = 0; j < count; j++, n++)
            if (out && n < capacity)
                out[n] = bytes[j];
    }
    return n;
}

struct sink {
    uint16_t *out;
    unsigned capacity, length;
};
static void put(struct sink *s, uint16_t c)
{
    if (s->length + 1 < s->capacity)
        s->out[s->length++] = c;
}
static void pad(struct sink *s, int count, uint16_t c)
{
    while (count-- > 0)
        put(s, c);
}

int ce_wformat(uint16_t *out, unsigned capacity, const uint16_t *format, const uintptr_t *args,
               unsigned count)
{
    struct sink s = {out, capacity, 0};
    unsigned arg = 0;
    if (!capacity)
        return 0;
    for (const uint16_t *f = format; *f;) {
        if (*f != '%') {
            put(&s, *f++);
            continue;
        }
        f++;
        if (*f == '%') {
            put(&s, *f++);
            continue;
        }
        bool left = false, zero = false, plus = false, space = false, alt = false;
        for (;; f++) {
            if (*f == '-')
                left = true;
            else if (*f == '0')
                zero = true;
            else if (*f == '+')
                plus = true;
            else if (*f == ' ')
                space = true;
            else if (*f == '#')
                alt = true;
            else
                break;
        }
        int width = 0, precision = -1;
        if (*f == '*') {
            if (arg == count)
                return -1;
            width = (int)(intptr_t)(int32_t)args[arg++];
            if (width < 0) {
                left = true;
                width = -width;
            }
            f++;
        } else
            for (; *f >= '0' && *f <= '9'; f++)
                if (width < 1000)
                    width = width * 10 + (*f - '0');
        if (*f == '.') {
            f++;
            precision = 0;
            if (*f == '*') {
                if (arg == count)
                    return -1;
                precision = (int)(int32_t)args[arg++];
                if (precision < 0)
                    precision = -1;
                f++;
            } else
                for (; *f >= '0' && *f <= '9'; f++)
                    if (precision < 1000)
                        precision = precision * 10 + (*f - '0');
        }
        int size = 0; /* -1 narrow (h), 1 wide (l, w) */
        if (*f == 'h') {
            size = -1;
            f++;
        } else if (*f == 'l' || *f == 'w') {
            size = 1;
            f++;
        }
        uint16_t type = *f;
        if (!type)
            return -1;
        f++;
        if (arg == count)
            return -1;
        uintptr_t value = args[arg++];
        if (type == 'c' || type == 'C') {
            uint16_t c = (uint16_t)value;
            if (size < 0 || (type == 'C' && size <= 0))
                c = ce_cp1252_decode((uint8_t)value);
            if (!left)
                pad(&s, width - 1, ' ');
            put(&s, c);
            if (left)
                pad(&s, width - 1, ' ');
        } else if (type == 's' || type == 'S') {
            bool narrow = size < 0 || (type == 'S' && size <= 0);
            static const char null_text[] = "(null)";
            const void *p = value ? (const void *)value : null_text;
            if (!value)
                narrow = true;
            int n = 0;
            while ((precision < 0 || n < precision) &&
                   (narrow ? ((const uint8_t *)p)[n] : ((const uint16_t *)p)[n]))
                n++;
            if (!left)
                pad(&s, width - n, ' ');
            for (int i = 0; i < n; i++)
                put(&s, narrow ? ce_cp1252_decode(((const uint8_t *)p)[i]) : ((const uint16_t *)p)[i]);
            if (left)
                pad(&s, width - n, ' ');
        } else if (type == 'd' || type == 'i' || type == 'u' || type == 'x' || type == 'X' || type == 'o' ||
                   type == 'p') {
            uint32_t n = (uint32_t)value;
            char sign = 0;
            if (type == 'd' || type == 'i') {
                if ((int32_t)n < 0) {
                    sign = '-';
                    n = 0u - n;
                } else if (plus)
                    sign = '+';
                else if (space)
                    sign = ' ';
            }
            if (type == 'p') {
                type = 'X';
                precision = 8;
            }
            unsigned base = type == 'o' ? 8 : (type == 'x' || type == 'X') ? 16 : 10;
            const char *digits = type == 'x' ? "0123456789abcdef" : "0123456789ABCDEF";
            char buffer[12];
            int length = 0;
            for (uint32_t v = n; v; v /= base)
                buffer[length++] = digits[v % base];
            if (!length && precision)
                buffer[length++] = '0';
            const char *prefix = "";
            if (alt && n && base == 16)
                prefix = type == 'x' ? "0x" : "0X";
            int prefix_length = sign ? 1 : (int)(prefix[0] ? 2 : 0);
            int zeros = precision > length ? precision - length : 0;
            if (alt && base == 8 && !zeros && (!length || buffer[length - 1] != '0'))
                zeros = 1;
            int total = prefix_length + zeros + length;
            if (zero && !left && precision < 0 && width > total) {
                zeros += width - total;
                total = width;
            }
            if (!left)
                pad(&s, width - total, ' ');
            if (sign)
                put(&s, (uint16_t)sign);
            for (const char *p = prefix; *p; p++)
                put(&s, (uint16_t)*p);
            pad(&s, zeros, '0');
            while (length)
                put(&s, (uint16_t)buffer[--length]);
            if (left)
                pad(&s, width - total, ' ');
        } else
            return -1;
    }
    out[s.length] = 0;
    return (int)s.length;
}
