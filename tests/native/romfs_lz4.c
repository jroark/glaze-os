/* kernel/romfs.c and kernel/lz4.c against archives from tools/mkromfs.py:
 * plain and LZ4, from NAND and from RAM (a RAM boot), every byte of every
 * file compared, the cache and reclaim, and corrupt containers and pages
 * refused rather than mis-read.
 *
 * usage: romfs-lz4-test PLAIN.img LZ4.img DIR (DIR holds the source files) */
#define GLAZE_HW_H
#define GLAZE_MIPS_H
#include <stdint.h>
#include <stdlib.h>
static uint8_t *sdram;
#define SDRAM_SIZE      (16u * 1024u * 1024u)
#define KSEG0(pa)       ((uintptr_t)(sdram + (pa)))
#define PHYS(va)        ((uintptr_t)((const uint8_t *)(va) - sdram))
#define NAND_PAGE_DATA  512u
#define ROMFS_OFFSET    0x40000u
#define FLASH_FS_OFFSET 0x500000u
#define PTE_CACHED      0u
#define PTE_V           0u
#include "../../kernel/romfs.c"
#include "../../kernel/lz4.c"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

const struct gz_bootinfo *ram_boot;
static struct gz_bootinfo boot_info;
static uint8_t *archive;            /* the NAND's romfs slot */
static size_t archive_size;
static unsigned reads, next_frame = 16, freed;
static uint8_t frame_used[4096];

void boot_nand_read(uint32_t page, void *buf)
{
    uint32_t off = (page - FIRST_NAND_PAGE) * NAND_PAGE_DATA;
    memset(buf, 0xFF, NAND_PAGE_DATA);
    if (off < archive_size) memcpy(buf, archive + off, MIN(NAND_PAGE_DATA, archive_size - off));
}
void nand_read_page(uint32_t page, void *buf) { reads++; boot_nand_read(page, buf); }
void *page_alloc_colour_raw(uint32_t va)
{
    for (unsigned n = 0; n < 4096; n++) {
        unsigned f = (next_frame + n) % 4096;
        if (f < 16 || frame_used[f] || (f & 1) != ((va >> 12) & 1)) continue;   /* colour: bit 12 */
        frame_used[f] = 1;
        next_frame = f + 1;
        return sdram + f * 4096;
    }
    return NULL;
}
void page_free(void *kva) { frame_used[((uint8_t *)kva - sdram) / 4096] = 0; freed++; }
void mm_sync_code(void *p, uint32_t n) { (void)p; (void)n; }
int as_map_page(struct addrspace *as, uint32_t va, uint32_t pa, uint32_t flags) { (void)as; (void)va; (void)pa; (void)flags; return 0; }
void kprintf(const char *f, ...) { (void)f; }

static uint8_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END); *size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*size + 1);
    assert(fread(p, 1, *size, f) == *size);
    fclose(f);
    return p;
}

static void reset(void)
{
    memset(table, 0, sizeof(table)); memset(cache, 0, sizeof(cache)); memset(owner, 0, sizeof(owner));
    memset(maps, 0, sizeof(maps)); memset(frame_used, 0, sizeof(frame_used));
    nfiles = npages = cached = hand = 0; ram_pa = 0; lz = false; ram_boot = NULL;
}

/* Every source file reads back byte for byte, and pages past the end are 0. */
static unsigned check_files(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    unsigned files = 0;
    assert(d);
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char path[512], name[64];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        snprintf(name, sizeof(name), "f/%s", de->d_name);
        size_t size;
        uint8_t *want = load(path, &size);
        const struct romfs_entry *e = romfs_lookup(name);
        assert(e && e->size == size && e->offset % PAGE_SIZE == 0);
        uint8_t *got = malloc(size + 1);
        assert(!romfs_read(e, 0, got, (uint32_t)size));
        assert(!memcmp(got, want, size));
        if (size % PAGE_SIZE) {
            const uint8_t *tail = romfs_ptr(e, (uint32_t)size);
            for (uint32_t i = size % PAGE_SIZE; i < PAGE_SIZE; i++) assert(tail[i - size % PAGE_SIZE] == 0);
        }
        /* A frame's colour follows its archive page. */
        for (uint32_t off = 0; off < size; off += PAGE_SIZE)
            assert(((romfs_frame(e, off) >> 12) & 1) == (((e->offset + off) >> 12) & 1));
        free(want); free(got);
        files++;
    }
    closedir(d);
    return files;
}

static void boot_nand(const char *path)
{
    reset();
    free(archive);
    archive = load(path, &archive_size);
    romfs_init();
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    sdram = calloc(1, SDRAM_SIZE);

    /* Plain archive from NAND. */
    boot_nand(argv[1]);
    assert(nfiles && !lz);
    unsigned files = check_files(argv[3]);
    assert(nfiles == files);

    /* LZ4 archive from NAND: the marker is gone, files have logical places. */
    boot_nand(argv[2]);
    assert(lz && nfiles == files && !romfs_lookup(".lz4"));
    uint32_t physical = (uint32_t)archive_size / PAGE_SIZE;
    assert(npages > physical);          /* it is compressed */
    reads = 0;
    assert(check_files(argv[3]) == files);
    assert(reads > 0);
    /* Cached pages aren't read again; reclaim frees them and they come back. */
    unsigned before = reads;
    check_files(argv[3]);
    assert(reads == before);
    uint32_t n = romfs_cache_pages();
    assert(n && romfs_reclaim(n) == n && romfs_cache_pages() == 0);
    assert(check_files(argv[3]) == files && reads > before);

    /* LZ4 archive in RAM (RAM boot): decoded through the cache as well. */
    reset();
    uint32_t pa = 8u * 1024 * 1024;
    memcpy(sdram + pa, archive, archive_size);
    boot_info.romfs_pa = pa;
    boot_info.romfs_size = (uint32_t)archive_size;
    ram_boot = &boot_info;
    romfs_init();
    assert(lz && nfiles == files);
    reads = 0;
    assert(check_files(argv[3]) == files && reads == 0 && romfs_cache_pages());

    /* A corrupt page fails cleanly: no frame is left behind. */
    boot_nand(argv[2]);
    const struct romfs_entry *e = romfs_entry_at(0);
    uint32_t idx = e->offset / PAGE_SIZE;
    for (; idx < npages && lz_len[idx] == PAGE_SIZE; idx++) ;
    assert(idx < npages);
    memset(archive + lz_off[idx], 0xFF, lz_len[idx]);   /* literal run lengths far past the end */
    unsigned freed_before = freed;
    assert(page_pa(idx) == 0 && freed == freed_before + 1 && !cache[idx]);

    /* A corrupt container index refuses the whole archive. */
    reset();
    free(archive);
    archive = load(argv[2], &archive_size);
    uint32_t count, off;
    memcpy(&count, archive + 4, 4);
    for (uint32_t i = 0; i < count; i++) {
        if (!memcmp(archive + 8 + i * 56, ".lz4", 5)) continue;
        memcpy(&off, archive + 8 + i * 56 + 48, 4);
        uint32_t pages;
        memcpy(&pages, archive + off + 8, 4);
        if (!pages) continue;                       /* the empty file has no index */
        uint32_t big = 0x7FFFFFFF;
        memcpy(archive + off + 12, &big, 4);        /* first page's offset */
        break;
    }
    romfs_init();
    assert(nfiles == 0);

    /* lz4_decode never writes past its output. */
    uint8_t out[8], bad[] = { 0xF0, 0xFF, 0x10 };
    assert(lz4_decode(bad, sizeof(bad), out, sizeof(out)) == -1);
    uint8_t ok[] = { 0x50, 'h', 'e', 'l', 'l', 'o' };
    assert(lz4_decode(ok, sizeof(ok), out, sizeof(out)) == 5 && !memcmp(out, "hello", 5));
    uint8_t far[] = { 0x10, 'a', 0x05, 0x00 };     /* offset before the start */
    assert(lz4_decode(far, sizeof(far), out, sizeof(out)) == -1);

    printf("romfs-lz4: %u files, %u pages in %u (LZ4), NAND and RAM, cache and reclaim, corrupt "
           "pages and indices refused PASS\n", files, npages, physical);
    return 0;
}
