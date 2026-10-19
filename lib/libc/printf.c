#include <glaze/sys.h>
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

static void emit_num(struct out *o, uint32_t v, unsigned base, int width, char pad, bool neg, bool upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[12];
    int n = 0;

    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    if (neg && pad == '0')
        emit(o, '-');
    else if (neg)
        tmp[n++] = '-';
    for (int w = n + (neg && pad == '0'); w < width; w++)
        emit(o, pad);
    while (n)
        emit(o, tmp[--n]);
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    struct out o = { buf, n, 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(&o, *fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0;
        bool left = false;
        if (*fmt == '-') {
            left = true;
            fmt++;
        }
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        if (*fmt == 'l')
            fmt++;
        switch (*fmt) {
        case 'd':
        case 'i': {
            int v = va_arg(ap, int);
            emit_num(&o, v < 0 ? -(uint32_t)v : (uint32_t)v, 10, width, pad, v < 0, false);
            break;
        }
        case 'u':
            emit_num(&o, va_arg(ap, uint32_t), 10, width, pad, false, false);
            break;
        case 'x':
        case 'X':
            emit_num(&o, va_arg(ap, uint32_t), 16, width, pad, false, *fmt == 'X');
            break;
        case 'p':
            emit(&o, '0');
            emit(&o, 'x');
            emit_num(&o, (uint32_t)va_arg(ap, void *), 16, 8, '0', false, false);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = (int)strlen(s);
            if (!left)
                for (int w = len; w < width; w++)
                    emit(&o, ' ');
            while (*s)
                emit(&o, *s++);
            if (left)
                for (int w = len; w < width; w++)
                    emit(&o, ' ');
            break;
        }
        case 'c':
            emit(&o, (char)va_arg(ap, int));
            break;
        case '%':
            emit(&o, '%');
            break;
        default:
            emit(&o, '?');
            break;
        }
    }
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
