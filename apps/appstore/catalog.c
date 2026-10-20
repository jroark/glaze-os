#include "catalog.h"
#include <string.h>

bool store_version_ok(const char *s)
{
    if (!*s || strlen(s) > 15)
        return false;
    int digits = 0;
    for (; *s; s++) {
        if (*s == '.') {
            if (!digits)
                return false;
            digits = 0;
        } else if (*s >= '0' && *s <= '9') {
            if (++digits > 6)
                return false;
        } else
            return false;
    }
    return digits != 0;
}
int store_version_cmp(const char *a, const char *b)
{
    while (*a || *b) {
        unsigned av = 0, bv = 0;
        while (*a >= '0' && *a <= '9')
            av = av * 10 + (*a++ - '0');
        while (*b >= '0' && *b <= '9')
            bv = bv * 10 + (*b++ - '0');
        if (av != bv)
            return av > bv ? 1 : -1;
        if (*a == '.')
            a++;
        if (*b == '.')
            b++;
    }
    return 0;
}
bool store_id_ok(const char *s)
{
    if (!*s || *s == '.' || *s == '-' || strlen(s) > 24 || strstr(s, ".."))
        return false;
    for (; *s; s++)
        if (!(*s >= 'a' && *s <= 'z') && !(*s >= '0' && *s <= '9') && *s != '.' && *s != '-')
            return false;
    return true;
}
bool store_path_ok(const char *s)
{
    if (!*s || *s == '/' || strstr(s, "..") || strstr(s, "//") || s[strlen(s) - 1] == '/')
        return false;
    for (; *s; s++)
        if (!(*s >= 'a' && *s <= 'z') && !(*s >= 'A' && *s <= 'Z') && !(*s >= '0' && *s <= '9') &&
            *s != '/' && *s != '.' && *s != '-' && *s != '_')
            return false;
    return true;
}
static bool hash_ok(const char *s)
{
    if (strlen(s) != 64)
        return false;
    for (; *s; s++)
        if (!(*s >= '0' && *s <= '9') && !(*s >= 'a' && *s <= 'f'))
            return false;
    return true;
}
static bool complete(const struct store_app *a, unsigned seen)
{
    return seen == 2047 && store_id_ok(a->id) && store_version_ok(a->version) && store_path_ok(a->package) &&
           store_path_ok(a->icon) && hash_ok(a->sha256) && hash_ok(a->icon_sha256) && a->size > 0 &&
           a->size <= STORE_PACKAGE_MAX;
}
bool store_parse(struct store_catalog *out, const char *data, size_t len)
{
    if (len < 9 || len > STORE_CATALOG_MAX || memcmp(data, "GZSTORE1\n", 9))
        return false;
    memset(out, 0, sizeof(*out));
    unsigned seen = 0;
    struct store_app *a = NULL;
    for (size_t i = 9; i < len;) {
        size_t end = i;
        while (end < len && data[end] != '\n') {
            if ((unsigned char)data[end] < 32 || (unsigned char)data[end] > 126)
                return false;
            end++;
        }
        size_t n = end - i;
        const char *line = data + i;
        i = end + (end < len);
        if (!n)
            continue;
        if (n == 5 && !memcmp(line, "[app]", 5)) {
            if ((a && !complete(a, seen)) || out->count == STORE_MAX_APPS)
                return false;
            a = &out->apps[out->count++];
            seen = 0;
            continue;
        }
        const char *eq = memchr(line, '=', n);
        if (!a || !eq || eq + 1 == line + n)
            return false;
        size_t k = eq - line, v = n - k - 1;
        char *dst = NULL;
        size_t cap = 0;
        unsigned bit = 0;
#define FIELD(key, member, index)                                                                            \
    if (k == sizeof(key) - 1 && !memcmp(line, key, k)) {                                                     \
        dst = a->member;                                                                                     \
        cap = sizeof(a->member);                                                                             \
        bit = 1u << index;                                                                                   \
    }
        FIELD("id", id, 0)
        else FIELD("name", name, 1) else FIELD("version", version, 2) else FIELD("developer", developer, 3) else FIELD(
            "summary", summary,
            4) else FIELD("description", description,
                          5) else FIELD("package", package,
                                        6) else FIELD("icon", icon,
                                                      7) else FIELD("sha256", sha256,
                                                                    8) else FIELD("icon_sha256", icon_sha256,
                                                                                  9) else if (k == 4 &&
                                                                                              !memcmp(line,
                                                                                                      "size",
                                                                                                      4))
        {
            bit = 1024;
            for (size_t j = 0; j < v; j++) {
                char c = eq[1 + j];
                if (c < '0' || c > '9' || a->size > (STORE_PACKAGE_MAX - (c - '0')) / 10u)
                    return false;
                a->size = a->size * 10 + c - '0';
            }
        }
        else return false;
#undef FIELD
        if (seen & bit)
            return false;
        seen |= bit;
        if (dst) {
            if (v >= cap)
                return false;
            memcpy(dst, eq + 1, v);
            dst[v] = 0;
        }
    }
    if (a && !complete(a, seen))
        return false;
    for (int i = 0; i < out->count; i++)
        for (int j = 0; j < i; j++)
            if (!strcmp(out->apps[i].id, out->apps[j].id))
                return false;
    return true;
}
static unsigned lower(unsigned c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}
static bool contains(const char *s, const char *q)
{
    if (!*q)
        return true;
    for (; *s; s++) {
        const char *a = s, *b = q;
        while (*a && *b && lower((unsigned char)*a) == lower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*b)
            return true;
    }
    return false;
}
bool store_matches(const struct store_app *a, const char *q)
{
    return contains(a->name, q) || contains(a->developer, q) || contains(a->summary, q) ||
           contains(a->description, q);
}
