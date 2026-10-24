/*
 * Native test of the file server's CompactFlash volume (servers/fs/card.c
 * over FatFs) on an image file standing in for the card: mounting, card
 * swaps and removal, long names, listing in pages, files across many
 * clusters, rename over an existing file, and a full volume.
 *
 * usage: ./fattest image [nand-volume-image]
 *
 * With a second FAT16 image, it also tests /NAND (nanddisk.c): the volume
 * placed in a RAM NAND model laid out like Windows CE's, read-only then
 * read/write, with every program checked to clear bits only.
 *
 * The image is changed in place; run fsck.fat -n on it afterwards. `make
 * fat-test` does that for fresh FAT12, FAT16 and FAT32 images.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "card.h"

static FILE *img;
static struct gz_cf_info card;
static int failures;

#define CHECK(cond, what) do { \
    if (cond) printf("fattest: PASS %s\n", what); \
    else { printf("fattest: FAIL %s (line %d)\n", what, __LINE__); failures++; } \
} while (0)

int card_raw_info(struct gz_cf_info *info)
{
    *info = card;
    return 0;
}

int card_raw_read(uint32_t lba, uint32_t count, void *buf)
{
    if (!card.present || count > GZ_CF_IO_MAX / GZ_CF_SECTOR || lba + count > card.sectors)
        return GZ_EINVAL;
    fseek(img, (long)lba * GZ_CF_SECTOR, SEEK_SET);
    return fread(buf, GZ_CF_SECTOR, count, img) == count ? 0 : GZ_EIO;
}

int card_raw_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (!card.present || count > GZ_CF_IO_MAX / GZ_CF_SECTOR || lba + count > card.sectors)
        return GZ_EINVAL;
    fseek(img, (long)lba * GZ_CF_SECTOR, SEEK_SET);
    return fwrite(buf, GZ_CF_SECTOR, count, img) == count ? 0 : GZ_EIO;
}

void card_log(const char *msg)
{
    printf("%s\n", msg);
}

DWORD get_fattime(void)
{
    return (DWORD)(2026 - 1980) << 25 | 10u << 21 | 19u << 16;
}

static void insert(bool present)
{
    card.present = present;
    card.generation++;
    card_check();
}

static int write_file(const char *path, const void *data, uint32_t len, uint32_t flags)
{
    struct card_file f;
    int rv = card_open(&f, path, FS_O_WRITE | flags);

    if (rv)
        return rv;
    int total = 0;
    while ((uint32_t)total < len) {
        int n = card_write(&f, (const uint8_t *)data + total, MIN(len - (uint32_t)total, FS_IO_MAX));
        if (n < 0) {
            card_close(&f);
            return n;
        }
        total += n;
    }
    rv = card_close(&f);
    return rv ? rv : total;
}

static int read_file(const char *path, void *data, uint32_t max)
{
    struct card_file f;
    int rv = card_open(&f, path, FS_O_READ), total = 0, n;

    if (rv)
        return rv;
    while ((uint32_t)total < max &&
           (n = card_read(&f, (uint8_t *)data + total, MIN(max - (uint32_t)total, FS_IO_MAX))) > 0)
        total += n;
    card_close(&f);
    return total;
}

/* ---- /NAND: a FAT volume in Windows CE's NAND layout ---------------------- */

#define NAND_PAGE   512u
#define NAND_PPB    32u
#define NAND_BYTES  (GZ_NAND_BLOCKS * NAND_PPB * NAND_PAGE)
#define CE_FAT_PAGE 0x1da0u     /* where the stock image's FAT16 volume starts */

static uint8_t *nand;
static int nand_overwrites, nand_erases;

int nand_raw_read(uint32_t page, void *buf)
{
    if (page >= NAND_BYTES / NAND_PAGE)
        return GZ_EINVAL;
    memcpy(buf, nand + page * NAND_PAGE, NAND_PAGE);
    return 0;
}

/* NAND semantics: a program only clears bits. */
int nand_raw_prog(uint32_t page, const void *buf)
{
    const uint8_t *src = buf;
    uint8_t *dst = nand + page * NAND_PAGE;

    if (page >= NAND_BYTES / NAND_PAGE || page < GZ_NAND_FIRST_WRITABLE * NAND_PPB)
        return GZ_EPERM;
    for (uint32_t i = 0; i < NAND_PAGE; i++) {
        nand_overwrites += (src[i] & ~dst[i]) != 0;
        dst[i] &= src[i];
    }
    return 0;
}

int nand_raw_erase(uint32_t block)
{
    if (block >= GZ_NAND_BLOCKS || block < GZ_NAND_FIRST_WRITABLE)
        return GZ_EPERM;
    memset(nand + block * NAND_PPB * NAND_PAGE, 0xFF, NAND_PPB * NAND_PAGE);
    nand_erases++;
    return 0;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* The volume image goes into a NAND like the stock one (a partition table,
 * the FAT volume as entry 3), is used through /NAND alongside the card, and
 * is written back to the file for fsck.fat. */
static void nand_test(const char *path, uint8_t *data, uint8_t *back, uint32_t len)
{
    FILE *vf = fopen(path, "r+b");
    struct fs_dirent ents[16];
    struct fs_stat st;
    struct fs_info info;
    bool more = false;

    if (!vf) {
        CHECK(false, "open NAND volume image");
        return;
    }
    fseek(vf, 0, SEEK_END);
    uint32_t pages = (uint32_t)(ftell(vf) / NAND_PAGE);
    nand = malloc(NAND_BYTES);
    memset(nand, 0xFF, NAND_BYTES);
    CHECK(CE_FAT_PAGE + pages <= NAND_BYTES / NAND_PAGE, "NAND volume fits");
    static const uint32_t table[4][2] = { { 0, 0x20 }, { 0x20, 0x80 }, { 0xa0, 0x1d00 }, { CE_FAT_PAGE, 0 } };
    for (int i = 0; i < 4; i++) {
        put32(nand + i * 16 + 8, table[i][0]);
        put32(nand + i * 16 + 12, i == 3 ? pages : table[i][1]);
    }
    fseek(vf, 0, SEEK_SET);
    CHECK(fread(nand + CE_FAT_PAGE * NAND_PAGE, NAND_PAGE, pages, vf) == pages, "read NAND volume image");

    CHECK(nand_status() == GZ_ENOENT && card_mount_nand(false) == 0 && nand_status() == 0, "NAND mount read-only");
    const char *root = card_path("/NAND"), *sub = card_path("/NAND/a/b");
    CHECK(root && !strcmp(root, "1:") && card_is_root(root) && card_volume(root) == NAND_DRIVE &&
          !strcmp(sub, "1:/a/b") && !card_is_root(sub) && !card_path("/NANDs"), "NAND card_path");
    CHECK(card_stat(root, &st) == 0 && st.type == FS_DIR, "NAND stat root");
    CHECK(card_list(root, 0, ents, 16, &more) > 0, "NAND list root");
    CHECK(write_file("1:/RO.TXT", "x", 1, FS_O_CREATE) == GZ_EPERM, "NAND read-only refuses writes");
    CHECK(card_statfs(root, &info) == 0 && info.mounted == 0 && info.total > 0, "NAND statfs");
    uint32_t free0 = info.total - info.used;

    CHECK(card_mount_nand(true) == 0, "NAND mount read/write");
    CHECK(card_mkdir("1:/glaze") == 0, "NAND mkdir");
    CHECK(write_file("1:/glaze/data.bin", data, len, FS_O_CREATE | FS_O_TRUNC) == (int)len, "NAND write");
    card_sync();
    CHECK(card_rename("1:/glaze/data.bin", "1:/glaze/moved.bin") == 0, "NAND rename");
    CHECK(card_rename("1:/glaze/moved.bin", "/moved.bin") == GZ_EINVAL, "no rename across volumes");
    card_sync();
    CHECK(card_mount_nand(true) == 0, "NAND remount");
    memset(back, 0, len);
    CHECK(read_file("1:/glaze/moved.bin", back, len) == (int)len && !memcmp(data, back, len),
          "NAND read back after remount");
    CHECK(card_stat("/FATTEST.TXT", &st) == 0 && st.size == 19, "card still mounted beside NAND");
    CHECK(card_remove("1:/glaze/moved.bin") == 0 && card_remove("1:/glaze") == 0, "NAND remove");
    CHECK(card_statfs(root, &info) == 0 && info.total - info.used == free0, "NAND space returned");
    CHECK(write_file("1:/GLAZE.TXT", "written by fattest\n", 19, FS_O_CREATE | FS_O_TRUNC) == 19,
          "leave GLAZE.TXT on NAND");
    card_sync();
    CHECK(nand_overwrites == 0 && nand_erases > 0, "NAND programs only erased bits");

    fseek(vf, 0, SEEK_SET);
    CHECK(fwrite(nand + CE_FAT_PAGE * NAND_PAGE, NAND_PAGE, pages, vf) == pages, "write NAND volume image");
    fclose(vf);
    free(nand);
}

int main(int argc, char **argv)
{
    static uint8_t data[300000], back[300000];
    struct fs_dirent ents[16];
    struct fs_stat st;
    struct fs_info info;
    struct card_file f;
    char name[64];
    bool more;

    if (argc < 2 || argc > 3 || !(img = fopen(argv[1], "r+b"))) {
        fprintf(stderr, "usage: %s image [nand-volume-image]\n", argv[0]);
        return 2;
    }
    fseek(img, 0, SEEK_END);
    card.sectors = (uint32_t)(ftell(img) / GZ_CF_SECTOR);
    strcpy(card.model, "image file");

    card_check();
    CHECK(card_status() == GZ_ENOENT && card_stat("", &st) == GZ_ENOENT, "no card");
    insert(true);
    CHECK(card_status() == 0, "mount");
    CHECK(card_path("/Card") && !*card_path("/Card") && !strcmp(card_path("/Card/a/b"), "/a/b") &&
          !card_path("/Cards") && !card_path("/Documents"), "card_path");
    CHECK(card_stat("", &st) == 0 && st.type == FS_DIR, "stat root");
    CHECK(card_statfs("", &info) == 0 && info.mounted == 0 && info.total > 0 && info.used <= info.total,
          "statfs");
    uint32_t free0 = info.total - info.used;

    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i * 31 + i / 997);
    CHECK(card_mkdir("/fattest") == 0 && card_mkdir("/fattest") == GZ_EEXIST, "mkdir");
    const char *lname = "/fattest/A long file name, with spaces and \xc3\xa9.data";
    CHECK(write_file(lname, data, sizeof(data), FS_O_CREATE | FS_O_TRUNC) == (int)sizeof(data) &&
          read_file(lname, back, sizeof(back)) == (int)sizeof(data) && !memcmp(data, back, sizeof(data)),
          "long name, 300000 bytes");
    CHECK(card_stat(lname, &st) == 0 && st.type == FS_FILE && st.size == sizeof(data), "stat file");
    CHECK(card_open(&f, "/fattest/none", FS_O_READ) == GZ_ENOENT, "open missing");
    CHECK(write_file(lname, "x", 1, FS_O_CREATE | FS_O_EXCL) == GZ_EEXIST, "create exclusive");

    for (int i = 0; i < 40; i++) {
        snprintf(name, sizeof(name), "/fattest/file %02d.txt", i);
        write_file(name, name, (uint32_t)strlen(name), FS_O_CREATE);
    }
    int seen = 0, n;
    for (uint32_t skip = 0;; skip += (uint32_t)n) {
        more = false;
        n = card_list("/fattest", skip, ents, 16, &more);
        if (n <= 0)
            break;
        seen += n;
        if (!more)
            break;
    }
    CHECK(seen == 41, "list 41 entries in pages of 16");

    CHECK(card_open(&f, "/fattest/file 00.txt", FS_O_READ | FS_O_WRITE) == 0 &&
          card_seek(&f, 0, FS_SEEK_END) == 20 && card_seek(&f, -4, FS_SEEK_CUR) == 16 &&
          card_seek(&f, -100, FS_SEEK_SET) == GZ_EINVAL && card_close(&f) == 0, "seek");
    CHECK(card_open(&f, "/fattest/file 00.txt", FS_O_READ | FS_O_WRITE) == 0 &&
          card_seek(&f, 4, FS_SEEK_SET) == 4 && card_set_end(&f) == 0 && card_close(&f) == 0 &&
          read_file("/fattest/file 00.txt", back, sizeof(back)) == 4 && !memcmp(back, "/fat", 4),
          "set-end truncates at cursor and preserves prefix");
    CHECK(card_open(&f, "/fattest/file 00.txt", FS_O_READ) == 0 && card_set_end(&f) < 0 &&
          card_close(&f) == 0, "set-end rejects read-only card files");
    CHECK(card_rename("/fattest/file 01.txt", "/fattest/file 02.txt") == 0 &&
          read_file("/fattest/file 02.txt", back, sizeof(back)) == 20 && !memcmp(back, "/fattest/file 01.txt", 20) &&
          card_stat("/fattest/file 01.txt", &st) == GZ_ENOENT, "rename over existing");
    CHECK(card_rename("/fattest/file 03.txt", "/fattest/FILE 03.TXT") == 0 &&
          card_stat("/fattest/file 03.txt", &st) == 0, "rename changing case");
    CHECK(card_remove("/fattest") == GZ_ENOTEMPTY, "remove non-empty dir");

    /* A swapped card: open handles go stale, the new card mounts. */
    CHECK(card_open(&f, lname, FS_O_READ) == 0, "open before swap");
    insert(true);
    CHECK(card_status() == 0 && card_read(&f, back, 16) == GZ_EIO, "stale handle after swap");
    card_close(&f);
    insert(false);
    CHECK(card_status() == GZ_ENOENT && card_stat("/fattest", &st) == GZ_ENOENT, "removed");
    insert(true);
    CHECK(card_stat(lname, &st) == 0 && st.size == sizeof(data), "remounted");

    /* Fill the volume, then free it again. */
    int i, rv = 0;
    for (i = 0; i < 1000 && rv >= 0; i++) {
        snprintf(name, sizeof(name), "/fattest/fill%03d.bin", i);
        rv = write_file(name, data, sizeof(data), FS_O_CREATE | FS_O_TRUNC);
    }
    CHECK(rv == GZ_ENOSPC, "volume full");
    CHECK(card_statfs("", &info) == 0 && info.total - info.used < sizeof(data), "statfs full");
    while (i-- > 0) {
        snprintf(name, sizeof(name), "/fattest/fill%03d.bin", i);
        card_remove(name);
    }
    bool removed = true;
    for (i = 0; i < 40; i++) {
        snprintf(name, sizeof(name), "/fattest/file %02d.txt", i);
        if (i != 1)             /* renamed over file 02 */
            removed &= card_remove(name) == 0;
    }
    CHECK(removed, "remove files");
    CHECK(card_remove(lname) == 0 && card_remove("/fattest") == 0, "remove dir");
    CHECK(card_statfs("", &info) == 0 && info.total - info.used == free0, "space returned");
    CHECK(write_file("/FATTEST.TXT", "written by fattest\n", 19, FS_O_CREATE | FS_O_TRUNC) == 19,
          "leave FATTEST.TXT");

    if (argc == 3)
        nand_test(argv[2], data, back, sizeof(data));
    fclose(img);
    printf("fattest: %s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures != 0;
}
