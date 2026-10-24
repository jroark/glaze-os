/* The FAT volumes: a CompactFlash card as /Card (FatFs on SYS_CF_*), and
 * in a RAM boot Windows CE's FAT partition in the NAND as /NAND
 * (nanddisk.c on SYS_NAND_*). */
#ifndef FS_CARD_H
#define FS_CARD_H

#include <glaze/fs.h>
#include "ff.h"
#include "diskio.h"

#define CARD_NAME   "Card"
#define CARD_PATH   "/" CARD_NAME
#define NAND_NAME   "NAND"
#define NAND_PATH   "/" NAND_NAME

/* FatFs drives. */
#define CARD_DRIVE  0
#define NAND_DRIVE  1

/* Platform hooks, like ftl.c's: SYS_CF_* in main.c, an image file in
 * host/fattest.c. FatFs's get_fattime() comes from there too. */
int card_raw_info(struct gz_cf_info *info);
int card_raw_read(uint32_t lba, uint32_t count, void *buf);
int card_raw_write(uint32_t lba, uint32_t count, const void *buf);
void card_log(const char *msg);

/* The FatFs path for a path under /Card ("" for the card's root) or /NAND
 * ("1:..."), or NULL for a path on the flash volume. The result is one of
 * a few static buffers, good for the request being served. */
const char *card_path(const char *path);
/* The drive of a card_path() result, and whether it is a volume's root. */
int card_volume(const char *fpath);
bool card_is_root(const char *fpath);

/* Samples the socket and mounts or unmounts the card to match. Files left
 * open on a removed or swapped card fail with GZ_EIO until closed. */
void card_check(void);
/* 0 when a FAT volume is mounted on the card, else the reason it isn't. */
int card_status(void);

/* Mounts Windows CE's FAT partition as /NAND (a RAM boot), read-only unless
 * writable. Returns 0 or the reason it isn't mounted, as nand_status(). */
int card_mount_nand(bool writable);
int nand_status(void);
/* Writes buffered NAND changes back (after each request). */
void card_sync(void);

struct card_file {
    FIL f;
    bool append;
};

int card_list(const char *path, uint32_t skip, struct fs_dirent *out, int max, bool *more);
int card_open(struct card_file *cf, const char *path, uint32_t flags);
int card_read(struct card_file *cf, void *buf, uint32_t len);
int card_write(struct card_file *cf, const void *buf, uint32_t len);
int card_seek(struct card_file *cf, int32_t off, uint32_t whence);
int card_set_end(struct card_file *cf);
int card_close(struct card_file *cf);
int card_stat(const char *path, struct fs_stat *st);
int card_mkdir(const char *path);
int card_remove(const char *path);
int card_rename(const char *from, const char *to);
int card_statfs(const char *path, struct fs_info *info);

/* ---- nanddisk.c: FatFs drive NAND_DRIVE --------------------------------- */

/* Platform hooks: SYS_NAND_* in main.c, a RAM model in host/fattest.c.
 * Pages are 512 bytes, 32 to an erase block. */
int nand_raw_read(uint32_t page, void *buf);
int nand_raw_prog(uint32_t page, const void *buf);
int nand_raw_erase(uint32_t block);

/* Finds the FAT partition in Windows CE's partition table. 0, or an error. */
int nand_disk_open(bool writable);
DSTATUS nand_disk_status(void);
DRESULT nand_disk_read(BYTE *buff, LBA_t sector, UINT count);
DRESULT nand_disk_write(const BYTE *buff, LBA_t sector, UINT count);
DRESULT nand_disk_ioctl(BYTE cmd, void *buff);
int nand_disk_flush(void);

#endif
