#include <string.h>
#include <strings.h>
#include <glaze/types.h>

void *memset(void *d, int c, size_t n)
{
    uint8_t *p = d;
    uint32_t w = (uint8_t)c * 0x01010101u;

    while (n && ((uintptr_t)p & 3)) {
        *p++ = (uint8_t)c;
        n--;
    }
    for (; n >= 4; n -= 4, p += 4)
        *(uint32_t *)p = w;
    while (n--)
        *p++ = (uint8_t)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;

    if ((((uintptr_t)dp | (uintptr_t)sp) & 3) == 0) {
        for (; n >= 4; n -= 4, dp += 4, sp += 4)
            *(uint32_t *)dp = *(const uint32_t *)sp;
    }
    while (n--)
        *dp++ = *sp++;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;

    for (; n; n--, x++, y++) {
        if (*x != *y)
            return *x - *y;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b || !*a)
            return (uint8_t)*a - (uint8_t)*b;
    }
    return 0;
}

char *strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;

    for (; i < n && s[i]; i++)
        d[i] = s[i];
    for (; i < n; i++)
        d[i] = 0;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;

    if (dp <= sp || dp >= sp + n)
        return memcpy(d, s, n);
    while (n--)
        dp[n] = sp[n];
    return d;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

size_t strlcpy(char *d, const char *s, size_t n)
{
    size_t len = strlen(s);

    if (n) {
        size_t c = len < n - 1 ? len : n - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return len;
}

size_t strlcat(char *d, const char *s, size_t n)
{
    size_t dl = strlen(d);

    if (dl >= n)
        return dl + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}

char *strcpy(char *d, const char *s)
{
    char *r = d;

    while ((*d++ = *s++))
        ;
    return r;
}

char *strcat(char *d, const char *s)
{
    strcpy(d + strlen(d), s);
    return d;
}

char *strncat(char *d, const char *s, size_t n)
{
    char *p = d + strlen(d);

    while (n-- && *s)
        *p++ = *s++;
    *p = 0;
    return d;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;

    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;

    for (; n; n--, p++) {
        if (*p == (unsigned char)c)
            return (void *)p;
    }
    return NULL;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;

    while (s[n] && strchr(accept, s[n]))
        n++;
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;

    while (s[n] && !strchr(reject, s[n]))
        n++;
    return n;
}

char *strstr(const char *h, const char *n)
{
    size_t len = strlen(n);

    for (; *h; h++) {
        if (!strncmp(h, n, len))
            return (char *)h;
    }
    return len ? NULL : (char *)h;
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;

    while (n < max && s[n])
        n++;
    return n;
}

static int fold(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int d = fold((unsigned char)*a) - fold((unsigned char)*b);
        if (d || !*a)
            return d;
    }
    return 0;
}

int strcasecmp(const char *a, const char *b)
{
    return strncasecmp(a, b, (size_t)-1);
}
