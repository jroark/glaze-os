/*
 * First-fit allocator with coalescing, grown in 64 KiB chunks from
 * SYS_MMAP_ANON. Blocks carry an 8-byte header; sizes are 8-byte aligned.
 * Requests of 64 KiB or more get a mapping of their own, marked by bit 0 of
 * the size, which free returns to the kernel; the chunks are never returned.
 * One lock covers the heap once the process has threads.
 */

#include <glaze/sys.h>
#include <glaze/thread.h>
#include <stdlib.h>
#include <string.h>

struct block {
    size_t size;                /* payload bytes */
    struct block *next_free;    /* valid only while free */
};

#define HDR         8u
#define CHUNK       (64u * 1024u)
#define MIN_SPLIT   32u
#define BIG         CHUNK
#define PAGE        4096u
#define MAPPED      1u          /* size bit: the block is its own mapping */

static struct block *free_list;
static struct gz_mutex lock = GZ_MUTEX_INIT;

static void insert_free(struct block *b)
{
    /* Address-ordered so neighbours can coalesce. */
    struct block **link = &free_list;

    while (*link && *link < b)
        link = &(*link)->next_free;
    b->next_free = *link;
    *link = b;

    struct block *n = b->next_free;
    if (n && (char *)b + HDR + b->size == (char *)n) {
        b->size += HDR + n->size;
        b->next_free = n->next_free;
    }
    if (link != &free_list) {
        struct block *prev = (struct block *)((char *)link - offsetof(struct block, next_free));
        if ((char *)prev + HDR + prev->size == (char *)b) {
            prev->size += HDR + b->size;
            prev->next_free = b->next_free;
        }
    }
}

static bool grow(size_t need)
{
    size_t len = ALIGN_UP(need + HDR, CHUNK);
    struct block *b = gz_mmap_anon(len);

    if (!b)
        return false;
    b->size = len - HDR;
    insert_free(b);
    return true;
}

/* Zeroed, since the kernel hands out zeroed pages. */
static void *map_big(size_t n)
{
    size_t len = ALIGN_UP(n + HDR, PAGE);
    if (len < n)
        return NULL;
    struct block *b = gz_mmap_anon(len);
    if (!b)
        return NULL;
    b->size = (len - HDR) | MAPPED;
    return (char *)b + HDR;
}

static void *malloc_unlocked(size_t n)
{
    if (!n)
        n = 1;
    n = ALIGN_UP(n, 8u);
    for (int attempt = 0; attempt < 2; attempt++) {
        for (struct block **link = &free_list; *link; link = &(*link)->next_free) {
            struct block *b = *link;
            if (b->size < n)
                continue;
            if (b->size >= n + HDR + MIN_SPLIT) {
                struct block *rest = (struct block *)((char *)b + HDR + n);
                rest->size = b->size - n - HDR;
                rest->next_free = b->next_free;
                *link = rest;
                b->size = n;
            } else {
                *link = b->next_free;
            }
            return (char *)b + HDR;
        }
        if (!grow(n))
            return NULL;
    }
    return NULL;
}

static void free_unlocked(void *p)
{
    if (p)
        insert_free((struct block *)((char *)p - HDR));
}

void *malloc(size_t n)
{
    if (n >= BIG)
        return map_big(n);
    gz_mutex_lock(&lock);
    void *p = malloc_unlocked(n);
    gz_mutex_unlock(&lock);
    return p;
}

void free(void *p)
{
    if (p && ((struct block *)((char *)p - HDR))->size & MAPPED) {
        gz_munmap_anon((char *)p - HDR);
        return;
    }
    gz_mutex_lock(&lock);
    free_unlocked(p);
    gz_mutex_unlock(&lock);
}

void *calloc(size_t n, size_t m)
{
    if (m && n > (size_t)-1 / m)
        return NULL;
    if (n * m >= BIG)
        return map_big(n * m);
    void *p = malloc(n * m);

    if (p)
        memset(p, 0, n * m);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p)
        return malloc(n);
    struct block *b = (struct block *)((char *)p - HDR);
    size_t size = b->size & ~(size_t)MAPPED;
    if (size >= n)
        return p;
    void *q = malloc(n);
    if (q) {
        memcpy(q, p, size);
        free(p);
    }
    return q;
}
