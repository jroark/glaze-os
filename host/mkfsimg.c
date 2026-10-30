/*
 * Builds the factory file-system region for the NAND image: formats a RAM
 * copy of the region with the file server's own flash stack (ftl.c, Dhara
 * and littlefs), creates the standard folders, copies a host directory tree
 * into it and writes the raw region out. tools/mk_nand.py places it at
 * FLASH_FS_OFFSET, so the first boot mounts it instead of formatting.
 *
 *   mkfsimg <dir> <blocks> <out> [/volume/path=file ...]
 *
 * The extra arguments add single build products, such as app packages,
 * that don't belong in the checked-in tree.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "folders.h"
#include "ftl.h"

static uint8_t *flash;
static uint32_t pages, blocks;
static lfs_t lfs;
static uint8_t factory_bad[128];
int flash_raw_is_bad(uint32_t block) { return (factory_bad[block/8] >> (block%8)) & 1; }

/* Erased NAND reads as 0xFF and programming only clears bits. */
int flash_raw_read(uint32_t page, uint8_t *buf)
{
    if (page >= pages)
        return -1;
    memcpy(buf, flash + (size_t)page * FTL_PAGE_SIZE, FTL_PAGE_SIZE);
    return 0;
}

int flash_raw_prog(uint32_t page, const uint8_t *buf)
{
    uint8_t *p = flash + (size_t)page * FTL_PAGE_SIZE;

    if (page >= pages)
        return -1;
    for (int i = 0; i < FTL_PAGE_SIZE; i++)
        p[i] &= buf[i];
    return 0;
}

int flash_raw_erase(uint32_t block)
{
    if (block >= blocks)
        return -1;
    memset(flash + (size_t)block * FTL_PAGES_PER_BLOCK * FTL_PAGE_SIZE, 0xFF,
           FTL_PAGES_PER_BLOCK * FTL_PAGE_SIZE);
    return 0;
}

void ftl_log(const char *msg)
{
    fprintf(stderr, "mkfsimg: %s\n", msg);
}

static void die(const char *what, const char *path, int err)
{
    fprintf(stderr, "mkfsimg: %s %s: error %d\n", what, path, err);
    exit(1);
}

static void copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    lfs_file_t f;
    uint8_t buf[4096];
    size_t n;
    int rv;

    if (!in)
        die("open", src, errno);
    if ((rv = lfs_file_open(&lfs, &f, dst, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC)))
        die("create", dst, rv);
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if ((rv = lfs_file_write(&lfs, &f, buf, n)) != (int)n)
            die("write", dst, rv);
    }
    if ((rv = lfs_file_close(&lfs, &f)))
        die("close", dst, rv);
    fclose(in);
}

/* Copies the tree under `src` into the volume folder `dst` ("" = root). */
static void copy_tree(const char *src, const char *dst)
{
    DIR *d = opendir(src);
    struct dirent *e;

    if (!d)
        die("open", src, errno);
    while ((e = readdir(d))) {
        char hpath[1024], vpath[LFS_NAME_MAX * 4];
        struct stat st;

        if (e->d_name[0] == '.')
            continue;
        snprintf(hpath, sizeof(hpath), "%s/%s", src, e->d_name);
        snprintf(vpath, sizeof(vpath), "%s/%s", dst, e->d_name);
        if (stat(hpath, &st))
            die("stat", hpath, errno);
        if (S_ISDIR(st.st_mode)) {
            int rv = lfs_mkdir(&lfs, vpath);
            if (rv && rv != LFS_ERR_EXIST)
                die("mkdir", vpath, rv);
            copy_tree(hpath, vpath);
        } else if (S_ISREG(st.st_mode)) {
            copy_file(hpath, vpath);
        }
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: mkfsimg <dir> <blocks> <out> [/volume/path=file ...]\n");
        return 2;
    }
    blocks = (uint32_t)strtoul(argv[2], NULL, 0);
    pages = blocks * FTL_PAGES_PER_BLOCK;
    size_t size = (size_t)pages * FTL_PAGE_SIZE;
    flash = malloc(size);
    if (!flash || !blocks)
        return 1;
    memset(flash, 0xFF, size);

    if (blocks > 1024) return 2;
    for (int i = 4; i < argc; i++) if (!strncmp(argv[i], "--bad-blocks=", 13)) {
        FILE *in = fopen(argv[i]+13, "rb");
        if (!in || fread(factory_bad,1,sizeof(factory_bad),in) != sizeof(factory_bad) || fgetc(in) != EOF)
            die("bad bitmap", argv[i], -1);
        fclose(in);
        for (unsigned b=blocks;b<1024;b++) if (flash_raw_is_bad(b)) die("bitmap range", argv[i], -1);
    }
    int rv = ftl_mount(&lfs, blocks);
    if (rv != 1)
        die("format", argv[3], rv);
    for (unsigned i = 0; i < sizeof(fs_standard_folders) / sizeof(fs_standard_folders[0]); i++)
        lfs_mkdir(&lfs, fs_standard_folders[i]);
    copy_tree(argv[1], "");
    for (int i = 4; i < argc; i++) {        /* build products, such as packages */
        if (!strncmp(argv[i], "--bad-blocks=", 13)) continue;
        char *eq = strchr(argv[i], '=');
        if (!eq || argv[i][0] != '/')
            die("bad file argument", argv[i], 0);
        *eq = 0;
        copy_file(eq + 1, argv[i]);
    }
    lfs_unmount(&lfs);

    FILE *out = fopen(argv[3], "wb");
    if (!out || fwrite(flash, 1, size, out) != size || fclose(out))
        die("write", argv[3], errno);
    return 0;
}
