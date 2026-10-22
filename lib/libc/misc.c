#include <glaze/sys.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int abs(int v)
{
    return v < 0 ? -v : v;
}

int atoi(const char *s)
{
    int sign = 1, v = 0;

    while (*s == ' ')
        s++;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return sign * v;
}

static int digit_val(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return 99;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    const char *p = s;
    unsigned long v = 0;
    bool neg = false, any = false, overflow = false;

    while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
        p++;
    if (*p == '-' || *p == '+')
        neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && digit_val(p[2]) < 16) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = *p == '0' ? 8 : 10;
    }
    for (int d; (d = digit_val(*p)) < base; p++) {
        if (v > (0xFFFFFFFFul - (unsigned long)d) / (unsigned long)base)
            overflow = true;
        v = v * (unsigned long)base + (unsigned long)d;
        any = true;
    }
    if (end)
        *end = (char *)(any ? p : s);
    if (overflow) {
        errno = ERANGE;
        return 0xFFFFFFFFul;
    }
    return neg ? -v : v;
}

long strtol(const char *s, char **end, int base)
{
    const char *p = s;

    while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
        p++;
    bool neg = *p == '-';
    unsigned long v = strtoul(p + (neg || *p == '+'), end, base);
    if (end && *end == p + (neg || *p == '+'))
        *end = (char *)s;
    if (v > 0x7FFFFFFFul + neg) {
        errno = ERANGE;
        return neg ? (long)0x80000000ul : 0x7FFFFFFF;
    }
    return neg ? -(long)v : (long)v;
}

char *strndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *d = malloc(len + 1);

    if (d) {
        memcpy(d, s, len);
        d[len] = 0;
    }
    return d;
}

char *strdup(const char *s)
{
    return strndup(s, (size_t)-1);
}

void __assert_fail(const char *expr, const char *file, int line)
{
    printf("assert: %s:%d: %s\n", file, line, expr);
    exit(1);
}

/* Insertion sort: lists in Glaze are tiny (notes, settings rows, mail).
 * Elements are swapped in place, so any element size works. */
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    char *a = base;

    for (size_t i = 1; i < n; i++) {
        for (size_t j = i; j > 0 && cmp(a + (j - 1) * size, a + j * size) > 0; j--) {
            char *p = a + (j - 1) * size, *q = a + j * size;
            for (size_t k = 0; k < size; k++) {
                char t = p[k];
                p[k] = q[k];
                q[k] = t;
            }
        }
    }
}
