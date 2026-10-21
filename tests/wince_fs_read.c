/* Read one file from a disposable emulator NAND for round-trip assertions.
 * Mount repairs, if any, affect only this RAM copy. The source stays read-only. */
#include "../servers/fs/ftl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *flash;
static uint32_t pages, blocks;
int flash_raw_read(uint32_t page, uint8_t *out)
{
    if (page >= pages) return -1;
    memcpy(out, flash + (size_t)page * FTL_PAGE_SIZE, FTL_PAGE_SIZE);
    return 0;
}
int flash_raw_prog(uint32_t page, const uint8_t *in)
{
    if (page >= pages) return -1;
    for (unsigned i = 0; i < FTL_PAGE_SIZE; i++) flash[(size_t)page * FTL_PAGE_SIZE + i] &= in[i];
    return 0;
}
int flash_raw_erase(uint32_t block)
{
    if (block >= blocks) return -1;
    memset(flash + (size_t)block * FTL_PAGE_SIZE * FTL_PAGES_PER_BLOCK, 255,
           FTL_PAGE_SIZE * FTL_PAGES_PER_BLOCK);
    return 0;
}
void ftl_log(const char *message) { fprintf(stderr, "wince-fs-read: %s\n", message); }
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *source = fopen(argv[1], "rb");
    if (!source || fseek(source, 0, SEEK_END)) return 2;
    long end = ftell(source);
    if (end != 16 * 1024 * 1024 || fseek(source, 0x500000, SEEK_SET)) return 2;
    size_t size = (size_t)end - 0x500000;
    flash = malloc(size);
    if (!flash || fread(flash, 1, size, source) != size) return 2;
    fclose(source);
    pages = (uint32_t)(size / FTL_PAGE_SIZE); blocks = pages / FTL_PAGES_PER_BLOCK;
    lfs_t lfs;
    if (ftl_mount(&lfs, blocks) != 0) return 2;
    lfs_file_t file;
    int err = lfs_file_open(&lfs, &file, argv[2], LFS_O_RDONLY);
    if (err) { fprintf(stderr, "wince-fs-read: %s: %d\n", argv[2], err); return 1; }
    uint8_t buffer[4096];
    for (;;) {
        lfs_ssize_t n = lfs_file_read(&lfs, &file, buffer, sizeof(buffer));
        if (n < 0) return 2;
        if (!n) break;
        if (fwrite(buffer, 1, (size_t)n, stdout) != (size_t)n) return 2;
    }
    lfs_file_close(&lfs, &file); lfs_unmount(&lfs); free(flash);
    return 0;
}
