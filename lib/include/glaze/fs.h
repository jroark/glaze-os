/*
 * File system: client API and the protocol of the file server (servers/fs),
 * which keeps a littlefs volume on the flash above the key/value store and
 * shows a CompactFlash card's FAT volume, when there is one, as /Card.
 *
 * A client sends FS_HELLO with its pid and gets a session: an index plus a
 * shared-memory buffer the server keeps for it. Paths and data travel in
 * that buffer; the message words carry the operation, the session and
 * small arguments. Sessions of exited processes are reused, along with
 * whatever files they left open.
 */
#ifndef GLAZE_FS_H
#define GLAZE_FS_H

#include <glaze/abi.h>

#define FS_PORT_NAME    "fs"
#define FS_BUF_SIZE     8192u
#define FS_IO_MAX       4096u       /* data bytes per request */
#define FS_PATH_MAX     128
#define FS_NAME_MAX     64

/* Requests: w0 = op, w1 = session; replies: w0 = result (>= 0 or error). */
enum {
    FS_HELLO = 1,           /* w1 = pid -> w1 = session, w2 = shm id */
    FS_STAT,                /* buf: path -> buf: struct fs_stat */
    FS_LIST,                /* buf: path, w2 = entries to skip -> count, w1 = more,
                               buf: struct fs_dirent[] */
    FS_OPEN,                /* buf: path, w2 = FS_O_* -> handle */
    FS_READ,                /* w2 = handle, w3 = len -> bytes, buf: data */
    FS_WRITE,               /* w2 = handle, w3 = len, buf: data -> bytes */
    FS_SEEK,                /* w2 = handle, w3 = offset, w4 = FS_SEEK_* -> position */
    FS_CLOSE,               /* w2 = handle */
    FS_MKDIR,               /* buf: path */
    FS_REMOVE,              /* buf: path (a file or an empty directory) */
    FS_RENAME,              /* buf: old path, then new path at FS_PATH_MAX */
    FS_STATFS,              /* w2 = 1: buf: path of the volume -> buf: struct fs_info */
    FS_FORMAT,              /* erase everything */
    FS_SET_END,             /* w2 = handle; truncate/extend at current position */
    FS_NAND_READ,           /* read-only CE logical NAND: w2 byte offset, w3 bytes <= FS_IO_MAX */
    FS_CARD_FORMAT,         /* CF-only FAT16 superfloppy: w2 generation, w3 sector count */
};

#define FS_O_READ       0x1
#define FS_O_WRITE      0x2
#define FS_O_CREATE     0x4
#define FS_O_TRUNC      0x8
#define FS_O_EXCL       0x10
#define FS_O_APPEND     0x20

enum { FS_SEEK_SET, FS_SEEK_CUR, FS_SEEK_END };

enum { FS_FILE = 1, FS_DIR = 2 };

struct fs_stat {
    uint32_t type;          /* FS_FILE or FS_DIR */
    uint32_t size;
};

struct fs_dirent {
    uint8_t type;
    uint8_t name_len;
    uint16_t reserved;
    uint32_t size;
    char name[FS_NAME_MAX];
};

struct fs_info {
    int32_t mounted;        /* 0, or the error that stopped the mount */
    uint32_t total, used;   /* bytes */
    uint32_t flash_blocks, bad_blocks;
    uint32_t ecc_corrected, ecc_failed;
};

/* ---- client API (libc) --------------------------------------------------- */

/* Read the original NAND through the RAM-boot CE block map/ECC path.
 * Refused by the kernel in ordinary NAND boots or with nand=off. */
int fs_nand_read(uint32_t offset, void *dst, uint32_t len);
int fs_open(const char *path, int flags);
int fs_read(int fd, void *buf, uint32_t len);
int fs_write(int fd, const void *buf, uint32_t len);
int fs_seek(int fd, int32_t offset, int whence);
/* Set the file's length to its current position; requires a writable handle. */
int fs_set_end(int fd);
int fs_close(int fd);
int fs_stat(const char *path, struct fs_stat *st);
/* Up to `max` entries of a directory, after skipping `skip`; returns the
 * count, or an error. */
int fs_list(const char *path, uint32_t skip, struct fs_dirent *out, int max);
int fs_mkdir(const char *path);
/* Creates a folder and any missing parents; an existing one is fine. */
int fs_mkdirs(const char *path);
int fs_remove(const char *path);
/* Removes a file, or a folder and everything in it. */
int fs_remove_all(const char *path);
int fs_rename(const char *from, const char *to);
int fs_statfs(struct fs_info *info);
/* The volume holding `path`: the flash, or a CompactFlash card at /Card
 * (whose flash_blocks and ECC counts are 0). */
int fs_statvfs(const char *path, struct fs_info *info);
int fs_format(void);
int fs_card_format(uint32_t generation, uint32_t sectors);

/* Whole-file helpers: bytes read (up to max) or written, or an error. */
int fs_read_file(const char *path, void *buf, uint32_t max);
int fs_write_file(const char *path, const void *buf, uint32_t len);

const char *fs_strerror(int err);

#endif
