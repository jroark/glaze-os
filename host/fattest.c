/*
 * Native test of the file server's CompactFlash volume (servers/fs/card.c
 * over FatFs) on an image file standing in for the card: mounting, card
 * swaps and removal, long names, listing in pages, files across many
 * clusters, rename over an existing file, and a full volume.
 *
 * usage: ./fattest image
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

int main(int argc, char **argv)
{
    static uint8_t data[300000], back[300000];
    struct fs_dirent ents[16];
    struct fs_stat st;
    struct fs_info info;
    struct card_file f;
    char name[64];
    bool more;

    if (argc != 2 || !(img = fopen(argv[1], "r+b"))) {
        fprintf(stderr, "usage: %s image\n", argv[0]);
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
    CHECK(card_statfs(&info) == 0 && info.mounted == 0 && info.total > 0 && info.used <= info.total,
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
    CHECK(card_statfs(&info) == 0 && info.total - info.used < sizeof(data), "statfs full");
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
    CHECK(card_statfs(&info) == 0 && info.total - info.used == free0, "space returned");
    CHECK(write_file("/FATTEST.TXT", "written by fattest\n", 19, FS_O_CREATE | FS_O_TRUNC) == 19,
          "leave FATTEST.TXT");

    fclose(img);
    printf("fattest: %s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures != 0;
}
