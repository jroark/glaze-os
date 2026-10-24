/*
 * Read-only file archive (tools/mkromfs.py), kept raw in NAND at ROMFS_OFFSET
 * and paged into RAM on demand.
 *
 *   header:  "GZRF" u32 count
 *   entries: char name[48], u32 offset, u32 size   (offset from archive start)
 *   data:    each file 4 KiB aligned so its pages can be mapped into user space
 *
 * Only the header and the entry table are read at boot. The archive's 4 KiB
 * pages come into a page cache on first use: a process touching a mapped
 * romfs file or a romfs program's shared text faults the page in
 * (romfs_fault, image.c), and the kernel reads ELF headers and private pages
 * through it. A cache frame's colour is its archive offset's bit 12, as when
 * the archive sat in the kernel image, so image.c's colour rules still hold.
 * Frames that no process maps are dropped when memory runs low
 * (romfs_reclaim, called by the page allocator); mapped ones are pinned by a
 * per-frame map count.
 *
 * In a RAM boot (glaze/bootinfo.h) the loader has put the whole archive in
 * reserved RAM at an 8 KiB-aligned address, so every page already has a
 * frame of the right colour: page_pa returns it, and nothing is cached,
 * counted or reclaimed.
 */

#include "kernel.h"
#include "hw.h"
#include "mips.h"
#include "proc.h"
#include "drivers/nand.h"
#include <glaze/bootinfo.h>

#define NPAGES          (SDRAM_SIZE / PAGE_SIZE)
#define NAND_PER_PAGE   (PAGE_SIZE / NAND_PAGE_DATA)
#define FIRST_NAND_PAGE (ROMFS_OFFSET / NAND_PAGE_DATA)
#define MAX_PAGES       ((FLASH_FS_OFFSET - ROMFS_OFFSET) / PAGE_SIZE)
#define MAX_FILES       256

static struct romfs_entry table[MAX_FILES];
static uint32_t nfiles, npages;
static uint16_t cache[MAX_PAGES];       /* archive page -> frame + 1 */
static uint16_t owner[NPAGES];          /* frame -> archive page + 1 */
static uint16_t maps[NPAGES];           /* frame -> mappings in address spaces */
static uint32_t cached, hand;
static uint32_t ram_pa;                 /* the archive in RAM; 0 from NAND */

/* A 512-byte block of the archive. */
static void read_block(uint32_t n, uint8_t *buf)
{
    if (ram_pa)
        memcpy(buf, (const uint8_t *)KSEG0(ram_pa) + n * NAND_PAGE_DATA, NAND_PAGE_DATA);
    else
        nand_read_page(FIRST_NAND_PAGE + n, buf);
}

void romfs_init(void)
{
    static uint8_t buf[NAND_PAGE_DATA] __attribute__((aligned(4)));
    uint32_t count, len, limit = MAX_PAGES;

    if (ram_boot) {
        ram_pa = ram_boot->romfs_pa;
        limit = MIN(limit, ALIGN_UP(ram_boot->romfs_size, PAGE_SIZE) / PAGE_SIZE);
    }
    read_block(0, buf);
    memcpy(&count, buf + 4, 4);
    len = count * sizeof(struct romfs_entry);
    if (memcmp(buf, "GZRF", 4) || count > MAX_FILES || 8 + len > limit * PAGE_SIZE) {
        kprintf("romfs: no archive in %s\n", ram_pa ? "RAM" : "NAND");
        return;
    }
    /* The table follows the 8-byte header; the first block is in buf. */
    for (uint32_t pos = 0; pos < 8 + len; pos += NAND_PAGE_DATA) {
        if (pos)
            read_block(pos / NAND_PAGE_DATA, buf);
        uint32_t from = MAX(pos, 8u), to = MIN(pos + NAND_PAGE_DATA, 8 + len);
        if (to > from)
            memcpy((uint8_t *)table + from - 8, buf + from - pos, to - from);
    }
    for (uint32_t i = 0; i < count; i++) {
        struct romfs_entry *e = &table[i];
        e->name[sizeof(e->name) - 1] = 0;
        uint32_t end = ALIGN_UP(e->offset + e->size, PAGE_SIZE) / PAGE_SIZE;
        if (e->offset % PAGE_SIZE || e->offset + e->size < e->offset || end > limit) {
            kprintf("romfs: bad entry %s\n", e->name);
            return;
        }
        npages = MAX(npages, end);
    }
    nfiles = count;
}

const struct romfs_entry *romfs_lookup(const char *name)
{
    if (*name == '/')
        name++;
    for (uint32_t i = 0; i < nfiles; i++) {
        if (!strncmp(table[i].name, name, sizeof(table[i].name)))
            return &table[i];
    }
    return NULL;
}

uint32_t romfs_index(const struct romfs_entry *e)
{
    return (uint32_t)(e - table);
}

const struct romfs_entry *romfs_entry_at(uint32_t index)
{
    return index < nfiles ? &table[index] : NULL;
}

/* The cache frame of an archive page, reading it from NAND if need be. */
static uint32_t page_pa(uint32_t idx)
{
    if (idx >= npages)
        return 0;
    if (ram_pa)
        return ram_pa + idx * PAGE_SIZE;
    if (cache[idx])
        return (uint32_t)(cache[idx] - 1) * PAGE_SIZE;
    uint8_t *page = page_alloc_colour_raw(idx * PAGE_SIZE);     /* all read below */
    if (!page)
        return 0;
    for (uint32_t i = 0; i < NAND_PER_PAGE; i++)
        nand_read_page(FIRST_NAND_PAGE + idx * NAND_PER_PAGE + i, page + i * NAND_PAGE_DATA);
    mm_sync_code(page, PAGE_SIZE);      /* programs run from these pages */
    uint32_t frame = PHYS(page) / PAGE_SIZE;
    cache[idx] = (uint16_t)(frame + 1);
    owner[frame] = (uint16_t)(idx + 1);
    cached++;
    return frame * PAGE_SIZE;
}

uint32_t romfs_frame(const struct romfs_entry *e, uint32_t off)
{
    return off < ALIGN_UP(e->size, PAGE_SIZE) ? page_pa((e->offset + off) / PAGE_SIZE) : 0;
}

uint32_t romfs_cached_frame(const struct romfs_entry *e, uint32_t off)
{
    uint32_t idx = (e->offset + off) / PAGE_SIZE;

    if (ram_pa)
        return page_pa(idx);
    return idx < npages && cache[idx] ? (uint32_t)(cache[idx] - 1) * PAGE_SIZE : 0;
}

/* Valid until the next page allocation, which may reclaim the page. */
const uint8_t *romfs_ptr(const struct romfs_entry *e, uint32_t off)
{
    uint32_t pa = romfs_frame(e, off & ~(PAGE_SIZE - 1));

    return pa ? (const uint8_t *)KSEG0(pa) + off % PAGE_SIZE : NULL;
}

int romfs_read(const struct romfs_entry *e, uint32_t off, void *dst, uint32_t len)
{
    while (len) {
        uint32_t n = MIN(len, PAGE_SIZE - off % PAGE_SIZE);
        const uint8_t *src = romfs_ptr(e, off);
        if (!src)
            return GZ_ENOMEM;
        memcpy(dst, src, n);
        off += n;
        dst = (uint8_t *)dst + n;
        len -= n;
    }
    return 0;
}

bool romfs_is_cache_frame(uint32_t pa)
{
    return pa / PAGE_SIZE < NPAGES && owner[pa / PAGE_SIZE];
}

void romfs_map_ref(uint32_t pa)
{
    if (romfs_is_cache_frame(pa))
        maps[pa / PAGE_SIZE]++;
}

void romfs_map_unref(uint32_t pa)
{
    uint32_t f = pa / PAGE_SIZE;

    if (romfs_is_cache_frame(pa) && maps[f])
        maps[f]--;
}

/* Frees up to n cached pages that nothing maps, oldest-first by archive
 * position from a moving hand. Returns the number freed. */
uint32_t romfs_reclaim(uint32_t n)
{
    uint32_t freed = 0;

    for (uint32_t i = 0; i < npages && freed < n; i++) {
        uint32_t idx = hand;
        hand = (hand + 1) % (npages ? npages : 1);
        if (!cache[idx])
            continue;
        uint32_t f = cache[idx] - 1u;
        if (maps[f])
            continue;
        cache[idx] = 0;
        owner[f] = 0;
        cached--;
        page_free((void *)KSEG0(f * PAGE_SIZE));
        freed++;
    }
    return freed;
}

uint32_t romfs_cache_pages(void)
{
    return cached;
}

uint32_t romfs_reclaimable_pages(void)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < npages; i++)
        n += cache[i] && !maps[cache[i] - 1u];
    return n;
}

/* A fault in a REGION_ROMFS mapping (sys_romfs_map): bring the page in. */
int romfs_fault(struct addrspace *as, struct region *r, uint32_t va)
{
    const struct romfs_entry *e = romfs_entry_at((uint32_t)r->shm_id);
    uint32_t off = (va & ~(PAGE_SIZE - 1)) - r->va;

    if (!e)
        return GZ_EINVAL;
    uint32_t pa = romfs_frame(e, off);
    if (!pa)
        return GZ_ENOMEM;
    int rv = as_map_page(as, va & ~(PAGE_SIZE - 1), pa, PTE_CACHED | PTE_V);
    if (!rv)
        romfs_map_ref(pa);
    return rv;
}

void romfs_list(void)
{
    kprintf("romfs: %u files, %u KiB in %s\n", nfiles, npages * 4, ram_pa ? "RAM" : "NAND");
}
