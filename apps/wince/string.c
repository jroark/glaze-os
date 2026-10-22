/* CE's legacy tokenizers use two arguments and thread-local continuation
 * state. The wide variants operate on 16-bit characters on every host. */
#include "runtime.h"

static bool contains(const WCHAR *set, WCHAR c)
{
    while (*set)
        if (*set++ == c)
            return true;
    return false;
}
CE_API(wcstok)
{
    if (!a1) {
        ce_error = 87;
        return 0;
    }
    void **context = ce_token_context(1);
    WCHAR *p = a0 ? PTR(WCHAR, a0) : *context;
    const WCHAR *delimiters = PTR(WCHAR, a1);
    while (p && *p && contains(delimiters, *p))
        p++;
    if (!p || !*p) {
        *context = NULL;
        return 0;
    }
    WCHAR *start = p;
    while (*p && !contains(delimiters, *p))
        p++;
    if (*p)
        *p++ = 0;
    *context = *p ? p : NULL;
    return WORD(start);
}
CE_API(strtok)
{
    if (!a1) {
        ce_error = 87;
        return 0;
    }
    void **context = ce_token_context(0);
    char *p = a0 ? PTR(char, a0) : *context;
    const char *delimiters = PTR(char, a1);
    while (p && *p && strchr(delimiters, *p))
        p++;
    if (!p || !*p) {
        *context = NULL;
        return 0;
    }
    char *start = p;
    while (*p && !strchr(delimiters, *p))
        p++;
    if (*p)
        *p++ = 0;
    *context = *p ? p : NULL;
    return WORD(start);
}
CE_API(wcsncat)
{
    WCHAR *out = PTR(WCHAR, a0), *src = PTR(WCHAR, a1);
    unsigned i = ce_wlen(out), n = 0;
    while (n < a2 && src[n]) {
        out[i + n] = src[n];
        n++;
    }
    out[i + n] = 0;
    return a0;
}
CE_API(_wcsdup)
{
    if (!a0)
        return 0;
    unsigned size = (ce_wlen(PTR(WCHAR, a0)) + 1) * 2;
    void *copy = malloc(size);
    if (copy)
        memcpy(copy, PTR(void, a0), size);
    return WORD(copy);
}
CE_API(wcspbrk)
{
    WCHAR *p = PTR(WCHAR, a0);
    while (*p) {
        if (contains(PTR(WCHAR, a1), *p))
            return WORD(p);
        p++;
    }
    return 0;
}
CE_API(_itow)
{
    if (!a1 || a2 < 2 || a2 > 36) {
        ce_error = 87;
        return 0;
    }
    WCHAR reverse[34], *out = PTR(WCHAR, a1);
    bool negative = a2 == 10 && (int32_t)a0 < 0;
    CW value = negative ? 0u - a0 : a0;
    unsigned count = 0, n = 0;
    do {
        unsigned digit = value % a2;
        reverse[count++] = (WCHAR)(digit < 10 ? '0' + digit : 'a' + digit - 10);
        value /= a2;
    } while (value);
    if (negative)
        out[n++] = '-';
    while (count)
        out[n++] = reverse[--count];
    out[n] = 0;
    return a1;
}
void *ce_resolve_string(const char *name)
{
    static const struct ce_export exports[] = {CE_EXPORT(wcstok),  CE_EXPORT(strtok),  CE_EXPORT(wcsncat),
                                               CE_EXPORT(_wcsdup), CE_EXPORT(wcspbrk), CE_EXPORT(_itow)};
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++)
        if (!strcmp(name, exports[i].name))
            return exports[i].address;
    return ce_resolve_scan(name);
}
