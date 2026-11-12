#include <glaze/sys.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct out {
    char *buf;
    size_t n, len;
};

static void emit(struct out *o, char c)
{
    if (o->len + 1 < o->n)
        o->buf[o->len] = c;
    o->len++;
}

/* Digits of v, padded to width. 32-bit values divide in 32 bits; only
 * %lld-sized ones call libc's __udivdi3. */
static void emit_num(struct out *o, uint64_t v, unsigned base, int width, char pad, bool left, bool neg,
                     bool upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];                   /* 22 octal digits for 2^64, and a sign */
    int n = 0;

    while (v >> 32) {
        tmp[n++] = digits[v % base];
        v /= base;
    }
    uint32_t v32 = (uint32_t)v;
    do {
        tmp[n++] = digits[v32 % base];
        v32 /= base;
    } while (v32);
    if (left)
        pad = ' ';
    if (neg && pad == '0')
        emit(o, '-');
    else if (neg)
        tmp[n++] = '-';
    int used = n + (neg && pad == '0');
    if (!left)
        for (int w = used; w < width; w++)
            emit(o, pad);
    while (n)
        emit(o, tmp[--n]);
    if (left)
        for (int w = used; w < width; w++)
            emit(o, ' ');
}

/* Length modifiers, each read with its own C type so that the arguments
 * after it stay in step (on o32 a long long also takes an aligned pair). */
enum { LEN_HH, LEN_H, LEN_INT, LEN_L, LEN_LL, LEN_J, LEN_Z, LEN_T };

static int64_t signed_arg(va_list *ap, int len)
{
    switch (len) {
    case LEN_HH: return (signed char)va_arg(*ap, int);
    case LEN_H:  return (short)va_arg(*ap, int);
    case LEN_L:  return va_arg(*ap, long);
    case LEN_LL: return va_arg(*ap, long long);
    case LEN_J:  return va_arg(*ap, intmax_t);
    case LEN_Z:  return (int64_t)va_arg(*ap, size_t);     /* %zd: ssize_t, the same size */
    case LEN_T:  return va_arg(*ap, ptrdiff_t);
    default:     return va_arg(*ap, int);
    }
}

static uint64_t unsigned_arg(va_list *ap, int len)
{
    switch (len) {
    case LEN_HH: return (unsigned char)va_arg(*ap, unsigned);
    case LEN_H:  return (unsigned short)va_arg(*ap, unsigned);
    case LEN_L:  return va_arg(*ap, unsigned long);
    case LEN_LL: return va_arg(*ap, unsigned long long);
    case LEN_J:  return va_arg(*ap, uintmax_t);
    case LEN_Z:  return va_arg(*ap, size_t);
    case LEN_T:  return (uint64_t)va_arg(*ap, ptrdiff_t);
    default:     return va_arg(*ap, unsigned);
    }
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap_in)
{
    struct out o = { buf, n, 0 };
    va_list ap;                     /* copied so helpers can take its address */

    va_copy(ap, ap_in);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(&o, *fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0;
        bool left = false;
        for (;; fmt++) {
            if (*fmt == '-')
                left = true;
            else if (*fmt == '0')
                pad = '0';
            else
                break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = true;
                width = -width;
            }
            fmt++;
        } else
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        int prec = -1;                  /* %.Ns and %.*s limit strings */
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }
        int len = LEN_INT;
        if (*fmt == 'h') {
            len = fmt[1] == 'h' ? LEN_HH : LEN_H;
            fmt += len == LEN_HH ? 2 : 1;
        } else if (*fmt == 'l') {
            len = fmt[1] == 'l' ? LEN_LL : LEN_L;
            fmt += len == LEN_LL ? 2 : 1;
        } else if (*fmt == 'j') {
            len = LEN_J;
            fmt++;
        } else if (*fmt == 'z') {
            len = LEN_Z;
            fmt++;
        } else if (*fmt == 't') {
            len = LEN_T;
            fmt++;
        }
        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t v = signed_arg(&ap, len);
            emit_num(&o, v < 0 ? -(uint64_t)v : (uint64_t)v, 10, width, pad, left, v < 0, false);
            break;
        }
        case 'u':
            emit_num(&o, unsigned_arg(&ap, len), 10, width, pad, left, false, false);
            break;
        case 'o':
            emit_num(&o, unsigned_arg(&ap, len), 8, width, pad, left, false, false);
            break;
        case 'x':
        case 'X':
            emit_num(&o, unsigned_arg(&ap, len), 16, width, pad, left, false, *fmt == 'X');
            break;
        case 'p':
            emit(&o, '0');
            emit(&o, 'x');
            emit_num(&o, (uintptr_t)va_arg(ap, void *), 16, 8, '0', false, false, false);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int slen = prec >= 0 ? (int)strnlen(s, (size_t)prec) : (int)strlen(s);
            if (!left)
                for (int w = slen; w < width; w++)
                    emit(&o, ' ');
            for (int i = 0; i < slen; i++)
                emit(&o, s[i]);
            if (left)
                for (int w = slen; w < width; w++)
                    emit(&o, ' ');
            break;
        }
        case 'c':
            emit(&o, (char)va_arg(ap, int));
            break;
        case '%':
            emit(&o, '%');
            break;
        case 0:
            fmt--;                      /* a trailing '%': stop at the NUL */
            break;
        default:
            emit(&o, '?');
            break;
        }
    }
    va_end(ap);
    if (n)
        buf[o.len < n ? o.len : n - 1] = 0;
    return (int)o.len;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gz_log(buf, (uint32_t)strlen(buf));
    return r;
}

int puts(const char *s)
{
    gz_log(s, (uint32_t)strlen(s));
    gz_log("\n", 1);
    return 0;
}
