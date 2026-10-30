/*
 * Flash translation for the file server: Dhara over raw NAND with in-band
 * BCH ECC and a bad-block table, presented to littlefs as a block device.
 * Platform independent; the raw flash functions below are supplied by the
 * file server (syscalls) or by the host test (a RAM model).
 */
#ifndef GLAZE_FTL_H
#define GLAZE_FTL_H

#include <stdint.h>
#include "lfs.h"

#define FTL_PAGE_SIZE       512
#define FTL_PAGES_PER_BLOCK 32

/* Raw flash in the file server's region; 0 on success. A program or erase
 * failure may also show up only as data that doesn't read back. */
/* 0 good, 1 factory/quarantined bad, negative if inventory unavailable. */
int flash_raw_is_bad(uint32_t block);
int flash_raw_read(uint32_t page, uint8_t *buf);
int flash_raw_prog(uint32_t page, const uint8_t *buf);
int flash_raw_erase(uint32_t block);
/* Diagnostic log line (no newline needed). */
void ftl_log(const char *msg);

struct ftl_stats {
    uint32_t blocks;            /* raw blocks in the region */
    uint32_t bad_blocks;
    uint32_t corrected;         /* pages repaired by ECC since mount */
    uint32_t uncorrectable;     /* pages ECC could not repair */
    uint32_t capacity;          /* bytes littlefs can use */
};

/* Mounts littlefs on the region, formatting blank flash. Returns 0, 1 if
 * it formatted, or a negative LFS_ERR_* code. */
int ftl_mount(lfs_t *lfs, uint32_t blocks);
/* Erases everything and mounts an empty filesystem (after ftl_mount). */
int ftl_format(lfs_t *lfs);
void ftl_get_stats(struct ftl_stats *st);

#endif
