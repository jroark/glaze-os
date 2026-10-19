/* The CompactFlash card volume: FatFs on SYS_CF_*, seen as /Card. */
#ifndef FS_CARD_H
#define FS_CARD_H

#include <glaze/fs.h>
#include "ff.h"

#define CARD_NAME   "Card"
#define CARD_PATH   "/" CARD_NAME

/* Platform hooks, like ftl.c's: SYS_CF_* in main.c, an image file in
 * host/fattest.c. FatFs's get_fattime() comes from there too. */
int card_raw_info(struct gz_cf_info *info);
int card_raw_read(uint32_t lba, uint32_t count, void *buf);
int card_raw_write(uint32_t lba, uint32_t count, const void *buf);
void card_log(const char *msg);

/* The FatFs path for a path under /Card ("" for the card's root), or NULL
 * for a path on the flash volume. */
const char *card_path(const char *path);

/* Samples the socket and mounts or unmounts the card to match. Files left
 * open on a removed or swapped card fail with GZ_EIO until closed. */
void card_check(void);
/* 0 when a FAT volume is mounted, else the reason it isn't. */
int card_status(void);

struct card_file {
    FIL f;
    bool append;
};

int card_list(const char *path, uint32_t skip, struct fs_dirent *out, int max, bool *more);
int card_open(struct card_file *cf, const char *path, uint32_t flags);
int card_read(struct card_file *cf, void *buf, uint32_t len);
int card_write(struct card_file *cf, const void *buf, uint32_t len);
int card_seek(struct card_file *cf, int32_t off, uint32_t whence);
int card_close(struct card_file *cf);
int card_stat(const char *path, struct fs_stat *st);
int card_mkdir(const char *path);
int card_remove(const char *path);
int card_rename(const char *from, const char *to);
int card_statfs(struct fs_info *info);

#endif
