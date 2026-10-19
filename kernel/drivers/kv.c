/*
 * Persistent key/value store on raw NAND (Notes, Settings).
 *
 * Two areas of 32 erase blocks alternate as a log. Page 0 of an area holds
 * its header (generation number); records follow from page 1, each starting
 * on a page boundary:
 *
 *   struct kv_rec header | value bytes ... (padded to a page)
 *
 * The newest record for a key wins; a tombstone (KV_DELETED) removes it.
 * When an area fills, live records are compacted into the other area, whose
 * header is written last so a torn compaction leaves the old area current.
 * Only page data is used: the emulator synthesizes OOB, so nothing is kept
 * there.
 */

#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"
#include "nand.h"

#define KV_FIRST_BLOCK  (0x500000u / (NAND_PAGE_DATA * NAND_PAGES_PER_BLOCK))
#define AREA_BLOCKS     32u
#define AREA_PAGES      (AREA_BLOCKS * NAND_PAGES_PER_BLOCK)
#define AREA_MAGIC      0x414B5A47u     /* "GZKA" */
#define REC_MAGIC       0x524B5A47u     /* "GZKR" */
#define KV_MAX_KEYS     128
#define KV_KEY_LEN      32
#define KV_MAX_VALUE    (16u * 1024u)
#define KV_DELETED      0xFFFFFFFFu

struct area_hdr {
    uint32_t magic, gen, gen_inv;
};

struct kv_rec {
    uint32_t magic;
    uint32_t vallen;            /* KV_DELETED for a tombstone */
    uint32_t crc;               /* over the value bytes */
    char key[KV_KEY_LEN];
};

struct kv_entry {
    char key[KV_KEY_LEN];
    uint32_t page;              /* area-relative page of the record */
    uint32_t vallen;
};

static struct kv_entry index_tab[KV_MAX_KEYS];
static uint32_t active_area, active_gen, write_page;
static bool kv_ready;

/* One record's worth of staging (header + max value, page rounded). */
static uint8_t staging[ALIGN_UP(sizeof(struct kv_rec) + KV_MAX_VALUE, NAND_PAGE_DATA)]
    __attribute__((aligned(4)));
static uint8_t page_buf[NAND_PAGE_DATA] __attribute__((aligned(4)));

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;

    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
    }
    return ~c;
}

static uint32_t area_page(uint32_t area, uint32_t page)
{
    return (KV_FIRST_BLOCK + area * AREA_BLOCKS) * NAND_PAGES_PER_BLOCK + page;
}

static uint32_t rec_pages(uint32_t vallen)
{
    uint32_t len = sizeof(struct kv_rec) + (vallen == KV_DELETED ? 0 : vallen);
    return (len + NAND_PAGE_DATA - 1) / NAND_PAGE_DATA;
}

static struct kv_entry *find(const char *key)
{
    for (int i = 0; i < KV_MAX_KEYS; i++) {
        if (index_tab[i].key[0] && !strncmp(index_tab[i].key, key, KV_KEY_LEN))
            return &index_tab[i];
    }
    return NULL;
}

static void index_set(const char *key, uint32_t page, uint32_t vallen)
{
    struct kv_entry *e = find(key);

    if (vallen == KV_DELETED) {
        if (e)
            memset(e, 0, sizeof(*e));
        return;
    }
    if (!e) {
        for (int i = 0; i < KV_MAX_KEYS && !e; i++) {
            if (!index_tab[i].key[0])
                e = &index_tab[i];
        }
        if (!e)
            return;
        strncpy(e->key, key, KV_KEY_LEN - 1);
    }
    e->page = page;
    e->vallen = vallen;
}

/* Read a whole record (header + value) from `area` into `staging`. */
static bool read_record(uint32_t area, uint32_t page, struct kv_rec **out)
{
    nand_read_page(area_page(area, page), staging);
    struct kv_rec *r = (struct kv_rec *)staging;
    if (r->magic != REC_MAGIC)
        return false;
    if (r->vallen != KV_DELETED && r->vallen > KV_MAX_VALUE)
        return false;
    uint32_t n = rec_pages(r->vallen);
    if (page + n > AREA_PAGES)
        return false;
    for (uint32_t i = 1; i < n; i++)
        nand_read_page(area_page(area, page + i), staging + i * NAND_PAGE_DATA);
    if (r->vallen != KV_DELETED && crc32(staging + sizeof(*r), r->vallen) != r->crc)
        return false;
    *out = r;
    return true;
}

static void write_record_pages(uint32_t area, uint32_t page, uint32_t npages)
{
    for (uint32_t i = 0; i < npages; i++)
        nand_program_page(area_page(area, page + i), staging + i * NAND_PAGE_DATA);
}

static bool read_area_hdr(uint32_t area, uint32_t *gen)
{
    nand_read_page(area_page(area, 0), page_buf);
    const struct area_hdr *h = (const struct area_hdr *)page_buf;
    if (h->magic != AREA_MAGIC || h->gen != ~h->gen_inv)
        return false;
    *gen = h->gen;
    return true;
}

static void erase_area(uint32_t area)
{
    for (uint32_t b = 0; b < AREA_BLOCKS; b++)
        nand_erase_block(KV_FIRST_BLOCK + area * AREA_BLOCKS + b);
}

static void write_area_hdr(uint32_t area, uint32_t gen)
{
    struct area_hdr *h = (struct area_hdr *)page_buf;

    memset(page_buf, 0xFF, sizeof(page_buf));
    h->magic = AREA_MAGIC;
    h->gen = gen;
    h->gen_inv = ~gen;
    nand_program_page(area_page(area, 0), page_buf);
}

static void scan_area(void)
{
    struct kv_rec *r;
    uint32_t page = 1;

    memset(index_tab, 0, sizeof(index_tab));
    while (page < AREA_PAGES && read_record(active_area, page, &r)) {
        char key[KV_KEY_LEN];
        strncpy(key, r->key, KV_KEY_LEN - 1);
        key[KV_KEY_LEN - 1] = 0;
        index_set(key, page, r->vallen);
        page += rec_pages(r->vallen);
    }
    write_page = page;
}

/* Copy live records into the other area and switch to it. */
static void compact(void)
{
    uint32_t to = active_area ^ 1u, page = 1;
    struct kv_rec *r;

    erase_area(to);
    for (int i = 0; i < KV_MAX_KEYS; i++) {
        struct kv_entry *e = &index_tab[i];
        if (!e->key[0] || !read_record(active_area, e->page, &r))
            continue;
        uint32_t n = rec_pages(r->vallen);
        write_record_pages(to, page, n);
        e->page = page;
        page += n;
    }
    write_area_hdr(to, active_gen + 1);
    active_area = to;
    active_gen++;
    write_page = page;
    kprintf("kv: compacted into area %u, %u pages used\n", to, page);
}

static int append(const char *key, const void *val, uint32_t vallen)
{
    struct kv_rec *r = (struct kv_rec *)staging;
    uint32_t n = rec_pages(vallen);

    if (write_page + n > AREA_PAGES) {
        compact();
        if (write_page + n > AREA_PAGES)
            return GZ_EFULL;
    }
    /* Build after a possible compact, which reuses the staging buffer. */
    memset(staging, 0xFF, n * NAND_PAGE_DATA);
    r->magic = REC_MAGIC;
    r->vallen = vallen;
    memset(r->key, 0, sizeof(r->key));
    strncpy(r->key, key, KV_KEY_LEN - 1);
    if (vallen != KV_DELETED) {
        memcpy(staging + sizeof(*r), val, vallen);
        r->crc = crc32(staging + sizeof(*r), vallen);
    } else {
        r->crc = 0;
    }
    write_record_pages(active_area, write_page, n);
    index_set(key, write_page, vallen);
    write_page += n;
    return 0;
}

void kv_init(void)
{
    uint32_t g0 = 0, g1 = 0;
    bool v0 = read_area_hdr(0, &g0), v1 = read_area_hdr(1, &g1);

    if (!v0 && !v1) {
        erase_area(0);
        write_area_hdr(0, 1);
        active_area = 0;
        active_gen = 1;
        kputs("kv: formatted new store\n");
    } else if (v0 && (!v1 || (int32_t)(g0 - g1) > 0)) {
        active_area = 0;
        active_gen = g0;
    } else {
        active_area = 1;
        active_gen = g1;
    }
    scan_area();
    kv_ready = true;

    int keys = 0;
    for (int i = 0; i < KV_MAX_KEYS; i++)
        keys += index_tab[i].key[0] != 0;
    kprintf("kv: area %u gen %u, %d keys, %u/%u pages used\n",
            active_area, active_gen, keys, write_page, AREA_PAGES);
}

static int user_key(struct addrspace *as, uint32_t ukey, char *key)
{
    int n = copy_str_from_as(as, key, ukey, KV_KEY_LEN);
    return (n <= 0) ? GZ_EINVAL : 0;
}

/* Returns the full value length (which may exceed `len`), or an error. */
int sys_kv_get(struct addrspace *as, uint32_t ukey, uint32_t ubuf, uint32_t len)
{
    char key[KV_KEY_LEN];
    struct kv_rec *r;

    if (!kv_ready || user_key(as, ukey, key))
        return GZ_EINVAL;
    struct kv_entry *e = find(key);
    if (!e)
        return GZ_ENOENT;
    if (!read_record(active_area, e->page, &r))
        return GZ_EINVAL;
    if (ubuf && len && copy_to_as(as, ubuf, staging + sizeof(*r), MIN(len, r->vallen)))
        return GZ_EINVAL;
    return (int)r->vallen;
}

/* len == KV_DELETED (0xFFFFFFFF) deletes the key. */
int sys_kv_put(struct addrspace *as, uint32_t ukey, uint32_t ubuf, uint32_t len)
{
    static uint8_t value[KV_MAX_VALUE];
    char key[KV_KEY_LEN];

    if (!kv_ready || user_key(as, ukey, key))
        return GZ_EINVAL;
    if (len == KV_DELETED)
        return find(key) ? append(key, NULL, KV_DELETED) : 0;
    if (len > KV_MAX_VALUE || copy_from_as(as, value, ubuf, len))
        return GZ_EINVAL;
    return append(key, value, len);
}
