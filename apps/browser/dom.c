/* Page arena, tag and keyword tables, DOM helpers. */

#include "web.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- arena ----------------------------------------------------------------- */

#define CHUNK_SIZE  (64u * 1024u)

struct arena_chunk {
    struct arena_chunk *next;
    size_t size, used;
    /* data follows, 8-aligned */
    uint64_t data[];
};

void *arena_alloc(struct arena *a, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    if (n > CHUNK_SIZE / 4) {
        /* Big blocks get a chunk of their own at the head of the list, so
         * the current chunk keeps filling. */
        struct arena_chunk *c = malloc(sizeof(*c) + n);
        if (!c) {
            a->failed = true;
            return NULL;
        }
        c->size = c->used = n;
        c->next = a->chunks;
        a->chunks = c;
        if (!a->cur)
            a->cur = c;
        a->used += n;
        memset(c->data, 0, n);
        return c->data;
    }
    for (;;) {
        struct arena_chunk *c = a->cur;
        if (c && c->size - c->used >= n) {
            void *p = (char *)c->data + c->used;
            c->used += n;
            a->used += n;
            memset(p, 0, n);
            return p;
        }
        if (c && c->next) {
            a->cur = c->next;       /* chunks kept by a reset */
            continue;
        }
        size_t size = CHUNK_SIZE - sizeof(struct arena_chunk);
        struct arena_chunk *nc = malloc(sizeof(*nc) + size);
        if (!nc) {
            a->failed = true;
            return NULL;
        }
        nc->size = size;
        nc->used = 0;
        nc->next = NULL;
        if (c)
            c->next = nc;
        else
            a->chunks = nc;
        a->cur = nc;
    }
}

char *arena_strndup(struct arena *a, const char *s, int n)
{
    char *d = arena_alloc(a, (size_t)n + 1);

    if (d) {
        memcpy(d, s, (size_t)n);
        d[n] = 0;
    }
    return d;
}

void arena_reset(struct arena *a)
{
    for (struct arena_chunk *c = a->chunks; c; c = c->next)
        c->used = 0;
    a->cur = a->chunks;
    a->used = 0;
    a->failed = false;
}

void arena_free(struct arena *a)
{
    while (a->chunks) {
        struct arena_chunk *n = a->chunks->next;
        free(a->chunks);
        a->chunks = n;
    }
    a->cur = NULL;
    a->used = 0;
    a->failed = false;
}

/* ---- names ------------------------------------------------------------------ */

static const char *const tag_names[T_COUNT] = {
    "",
#define X(id, name) name,
    TAG_LIST(X)
#undef X
};

static const char *const kw_names[K_COUNT] = {
    "",
#define X(id, name) name,
    KW_LIST(X)
#undef X
};

/* Lowercase-insensitive lookup in a name table, hashed on first use. */
struct name_table {
    const char *const *names;
    int count;
    int16_t slots[512];
    bool built;
};

static uint32_t hash_ci(const char *s, int len)
{
    uint32_t h = 2166136261u;

    for (int i = 0; i < len; i++) {
        unsigned c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z')
            c += 32;
        h = (h ^ c) * 16777619u;
    }
    return h;
}

static int table_lookup(struct name_table *t, const char *s, int len)
{
    if (!t->built) {
        memset(t->slots, 0, sizeof(t->slots));
        for (int i = 1; i < t->count; i++) {
            uint32_t h = hash_ci(t->names[i], (int)strlen(t->names[i])) & 511;
            while (t->slots[h])
                h = (h + 1) & 511;
            t->slots[h] = (int16_t)i;
        }
        t->built = true;
    }
    uint32_t h = hash_ci(s, len) & 511;
    while (t->slots[h]) {
        const char *n = t->names[t->slots[h]];
        if ((int)strlen(n) == len && !strncasecmp(n, s, (size_t)len))
            return t->slots[h];
        h = (h + 1) & 511;
    }
    return 0;
}

static struct name_table tags = { tag_names, T_COUNT, { 0 }, false };
static struct name_table kws = { kw_names, K_COUNT, { 0 }, false };

int tag_lookup(const char *name, int len)
{
    return table_lookup(&tags, name, len);
}

const char *tag_name(int tag)
{
    return tag > 0 && tag < T_COUNT ? tag_names[tag] : "";
}

int kw_lookup(const char *s, int len)
{
    return table_lookup(&kws, s, len);
}

const char *web_strerror(int err)
{
    switch (err) {
    case WE_NETDOWN: return "You are not connected to the Internet.";
    case WE_DNS: return "The server could not be found.";
    case WE_CONNECT: return "The server refused the connection.";
    case WE_TIMEOUT: return "The server stopped responding.";
    case WE_RESET: return "The connection was lost.";
    case WE_PROTOCOL: return "The server sent a response that could not be understood.";
    case WE_TOO_BIG: return "The page is too large to open.";
    case WE_HTTPS: return "Secure (https) pages are not supported yet.";
    case WE_SCHEME: return "This kind of address is not supported.";
    case WE_NOTFOUND: return "The file could not be found.";
    case WE_NOMEM: return "There is not enough memory to open the page.";
    case WE_REDIRECTS: return "Too many redirects.";
    case WE_BADURL: return "The address is not valid.";
    case WE_DECODE: return "The data could not be decoded.";
    default: return "The page could not be opened.";
    }
}

/* ---- nodes ------------------------------------------------------------------ */

const char *node_attr(const struct node *n, const char *name)
{
    if (!n || n->type != N_ELEM)
        return NULL;
    for (const struct attr *a = n->attrs; a; a = a->next)
        if (!strcmp(a->name, name))
            return a->value;
    return NULL;
}

bool node_has_class(const struct node *n, const char *cls, int len)
{
    const char *c = node_attr(n, "class");

    if (!c)
        return false;
    while (*c) {
        while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r' || *c == '\f')
            c++;
        const char *s = c;
        while (*c && *c != ' ' && *c != '\t' && *c != '\n' && *c != '\r' && *c != '\f')
            c++;
        if (c - s == len && !memcmp(s, cls, (size_t)len))
            return true;
    }
    return false;
}

struct node *node_first_elem(const struct node *n)
{
    for (struct node *c = n->first; c; c = c->next)
        if (c->type == N_ELEM)
            return c;
    return NULL;
}

struct node *node_next(const struct node *n, const struct node *root, bool skip_children)
{
    if (!skip_children && n->first)
        return n->first;
    while (n && n != root) {
        if (n->next)
            return n->next;
        n = n->parent;
    }
    return NULL;
}

char *node_text(struct arena *a, const struct node *n)
{
    int len = 0;

    for (const struct node *c = n; c; c = node_next(c, n, false))
        if (c->type == N_TEXT)
            len += c->len;
    char *out = arena_alloc(a, (size_t)len + 1);
    if (!out)
        return NULL;
    /* Collapse whitespace and trim. */
    int o = 0;
    bool space = false;
    for (const struct node *c = n; c; c = node_next(c, n, false)) {
        if (c->type != N_TEXT)
            continue;
        for (int i = 0; i < c->len; i++) {
            char ch = c->text[i];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f') {
                space = o > 0;
                continue;
            }
            if (space)
                out[o++] = ' ';
            space = false;
            out[o++] = ch;
        }
    }
    out[o] = 0;
    return out;
}
