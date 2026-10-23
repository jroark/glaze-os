/* App-private registry model. No Glaze or original-OS registry is shared.
 * GZCE value records remain readable; a GZCK suffix preserves empty keys. */
#include "registry.h"
#include "pe.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool root(const char *s)
{
    return !strcasecmp(s, "HKCR") || !strcasecmp(s, "HKCU") || !strcasecmp(s, "HKLM") ||
           !strcasecmp(s, "HKU");
}
static uint32_t encoded_size(const struct ce_registry *r)
{
    uint32_t n = 16;
    for (unsigned i = 0; i < r->value_count; i++)
        n += 18 + strlen(r->values[i].key) + strlen(r->values[i].name) + r->values[i].size;
    for (unsigned i = 0; i < r->key_count; i++)
        n += 5 + strlen(r->keys[i]);
    return n;
}
static bool below(const char *s, const char *parent)
{
    size_t n = strlen(parent);
    return strlen(s) > n && !strncasecmp(s, parent, n) && s[n] == '\\';
}
static bool valid_key(const char *s)
{
    char r[5];
    const char *slash = strchr(s, '\\');
    size_t n = slash ? (size_t)(slash - s) : strlen(s);
    if (n > 4 || strlen(s) >= 256)
        return false;
    memcpy(r, s, n);
    r[n] = 0;
    if (!root(r))
        return false;
    for (const char *p = slash; p && *p; p++)
        if (*p == '\\' && (!p[1] || p[1] == '\\'))
            return false;
    return true;
}
void ce_reg_clear(struct ce_registry *r)
{
    for (unsigned i = 0; i < r->value_count; i++)
        free(r->values[i].bytes);
    memset(r, 0, sizeof(*r));
}
bool ce_reg_exists(const struct ce_registry *r, const char *key)
{
    if (root(key))
        return true;
    for (unsigned i = 0; i < r->key_count; i++)
        if (!strcasecmp(r->keys[i], key))
            return true;
    return false;
}
int ce_reg_create(struct ce_registry *r, const char *key)
{
    if (!valid_key(key))
        return 87;
    char path[256];
    memcpy(path, key, strlen(key) + 1);
    unsigned missing = 0, extra = 0;
    for (char *p = path;; p++) {
        if (*p && *p != '\\')
            continue;
        char saved = *p;
        *p = 0;
        if (!ce_reg_exists(r, path)) {
            missing++;
            extra += 5 + strlen(path);
        }
        *p = saved;
        if (!saved)
            break;
    }
    if (missing > CE_REG_KEYS - r->key_count || extra > CE_REG_FILE_MAX - encoded_size(r))
        return 8;
    for (char *p = path;; p++) {
        if (*p && *p != '\\')
            continue;
        char saved = *p;
        *p = 0;
        if (!ce_reg_exists(r, path)) {
            memcpy(r->keys[r->key_count++], path, strlen(path) + 1);
        }
        *p = saved;
        if (!saved)
            break;
    }
    return 0;
}
const char *ce_reg_child(const struct ce_registry *r, const char *key, unsigned index)
{
    size_t n = strlen(key);
    for (unsigned i = 0; i < r->key_count; i++)
        if (below(r->keys[i], key) && !strchr(r->keys[i] + n + 1, '\\')) {
            if (!index--)
                return r->keys[i] + n + 1;
        }
    return NULL;
}
struct ce_reg_value *ce_reg_value_at(struct ce_registry *r, const char *key, unsigned index)
{
    for (unsigned i = 0; i < r->value_count; i++)
        if (!strcasecmp(r->values[i].key, key) && !index--)
            return &r->values[i];
    return NULL;
}
struct ce_reg_value *ce_reg_find(struct ce_registry *r, const char *key, const char *name)
{
    for (unsigned i = 0; i < r->value_count; i++)
        if (!strcasecmp(r->values[i].key, key) && !strcasecmp(r->values[i].name, name))
            return &r->values[i];
    return NULL;
}
int ce_reg_set(struct ce_registry *r, const char *key, const char *name, uint32_t type, const void *data,
               uint32_t size)
{
    if (!ce_reg_exists(r, key))
        return 2;
    if (strlen(name) >= 128 || size > 65536 || (size && !data))
        return 87;
    struct ce_reg_value *v = ce_reg_find(r, key, name);
    if (!v && r->value_count == CE_REG_VALUES)
        return 8;
    uint32_t next = encoded_size(r);
    if (v)
        next -= 18 + strlen(v->key) + strlen(v->name) + v->size;
    next += 18 + strlen(key) + strlen(name) + size;
    if (next > CE_REG_FILE_MAX)
        return 8;
    uint8_t *copy = malloc(size ? size : 1);
    if (!copy)
        return 8;
    if (size)
        memcpy(copy, data, size);
    if (!v)
        v = &r->values[r->value_count++];
    free(v->bytes);
    memcpy(v->key, key, strlen(key) + 1);
    memcpy(v->name, name, strlen(name) + 1);
    v->type = type;
    v->size = size;
    v->bytes = copy;
    return 0;
}
int ce_reg_remove_value(struct ce_registry *r, const char *key, const char *name)
{
    struct ce_reg_value *v = ce_reg_find(r, key, name);
    if (!v)
        return 2;
    free(v->bytes);
    *v = r->values[--r->value_count];
    memset(&r->values[r->value_count], 0, sizeof(*v));
    return 0;
}
int ce_reg_delete(struct ce_registry *r, const char *key)
{
    if (root(key))
        return 5;
    if (!ce_reg_exists(r, key))
        return 2;
    if (ce_reg_child(r, key, 0))
        return 5;
    while (ce_reg_value_at(r, key, 0)) {
        char name[128];
        strcpy(name, ce_reg_value_at(r, key, 0)->name);
        ce_reg_remove_value(r, key, name);
    }
    for (unsigned i = 0; i < r->key_count; i++)
        if (!strcasecmp(r->keys[i], key)) {
            r->key_count--;
            if (i < r->key_count)
                memmove(r->keys[i], r->keys[i + 1], (r->key_count - i) * sizeof(r->keys[0]));
            return 0;
        }
    return 2;
}
/* Windows CE's RegDeleteKey also removes the key's subkeys and their
 * values. The longest descendant never has children of its own. */
int ce_reg_delete_tree(struct ce_registry *r, const char *key)
{
    if (root(key))
        return 5;
    if (!ce_reg_exists(r, key))
        return 2;
    for (;;) {
        int deepest = -1;
        for (unsigned i = 0; i < r->key_count; i++)
            if (below(r->keys[i], key) && (deepest < 0 || strlen(r->keys[i]) > strlen(r->keys[deepest])))
                deepest = (int)i;
        if (deepest < 0)
            break;
        char child[256];
        strcpy(child, r->keys[deepest]);
        int error = ce_reg_delete(r, child);
        if (error)
            return error;
    }
    return ce_reg_delete(r, key);
}
static const char *name_at(const uint8_t *p, size_t size, size_t pos, uint32_t n, unsigned max)
{
    if (!n || n > max || pos > size || n > size - pos || p[pos + n - 1] || memchr(p + pos, 0, n - 1))
        return NULL;
    return (const char *)p + pos;
}
bool ce_reg_decode(struct ce_registry *r, const void *data, size_t size)
{
    const uint8_t *p = data;
    if (size < 8 || size > CE_REG_FILE_MAX || memcmp(p, "GZCE", 4))
        return false;
    struct ce_registry *tmp = calloc(1, sizeof(*tmp));
    if (!tmp)
        return false;
    uint32_t count = ce_u32(p + 4);
    size_t pos = 8;
    bool ok = count <= CE_REG_VALUES;
    for (unsigned i = 0; ok && i < count; i++) {
        if (pos > size || size - pos < 16) {
            ok = false;
            break;
        }
        uint32_t k = ce_u32(p + pos), n = ce_u32(p + pos + 4);
        uint32_t type = ce_u32(p + pos + 8), bytes = ce_u32(p + pos + 12);
        pos += 16;
        const char *key = name_at(p, size, pos, k, 256);
        if (!key) {
            ok = false;
            break;
        }
        pos += k;
        const char *name = name_at(p, size, pos, n, 128);
        if (!name) {
            ok = false;
            break;
        }
        pos += n;
        if (bytes > size - pos || bytes > 65536 || ce_reg_find(tmp, key, name) || ce_reg_create(tmp, key) ||
            ce_reg_set(tmp, key, name, type, p + pos, bytes)) {
            ok = false;
            break;
        }
        pos += bytes;
    }
    if (ok && pos < size) {
        if (size - pos < 8 || memcmp(p + pos, "GZCK", 4))
            ok = false;
        else {
            count = ce_u32(p + pos + 4);
            pos += 8;
            if (count > CE_REG_KEYS)
                ok = false;
            for (unsigned i = 0; ok && i < count; i++) {
                if (size - pos < 4) {
                    ok = false;
                    break;
                }
                uint32_t n = ce_u32(p + pos);
                pos += 4;
                const char *key = name_at(p, size, pos, n, 256);
                if (!key || ce_reg_create(tmp, key)) {
                    ok = false;
                    break;
                }
                pos += n;
            }
        }
    }
    ok = ok && pos == size;
    if (ok) {
        ce_reg_clear(r);
        *r = *tmp;
    } else
        ce_reg_clear(tmp);
    free(tmp);
    return ok;
}
uint8_t *ce_reg_encode(const struct ce_registry *r, uint32_t *size)
{
    uint32_t n = encoded_size(r);
    if (n > CE_REG_FILE_MAX)
        return NULL;
    uint8_t *p = malloc(n);
    if (!p)
        return NULL;
    memcpy(p, "GZCE", 4);
    ce_put32(p + 4, r->value_count);
    uint32_t pos = 8;
    for (unsigned i = 0; i < r->value_count; i++) {
        const struct ce_reg_value *v = &r->values[i];
        unsigned k = strlen(v->key) + 1, s = strlen(v->name) + 1;
        ce_put32(p + pos, k);
        ce_put32(p + pos + 4, s);
        ce_put32(p + pos + 8, v->type);
        ce_put32(p + pos + 12, v->size);
        pos += 16;
        memcpy(p + pos, v->key, k);
        pos += k;
        memcpy(p + pos, v->name, s);
        pos += s;
        memcpy(p + pos, v->bytes, v->size);
        pos += v->size;
    }
    memcpy(p + pos, "GZCK", 4);
    ce_put32(p + pos + 4, r->key_count);
    pos += 8;
    for (unsigned i = 0; i < r->key_count; i++) {
        unsigned k = strlen(r->keys[i]) + 1;
        ce_put32(p + pos, k);
        pos += 4;
        memcpy(p + pos, r->keys[i], k);
        pos += k;
    }
    *size = pos;
    return p;
}
