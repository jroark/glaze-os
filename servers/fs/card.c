/*
 * The CompactFlash card as /Card: FatFs (FAT12/16/32, long names in UTF-8)
 * over the kernel's ATA sectors, mounted whichever way the card is
 * formatted (a partition table's first FAT partition, or a "superfloppy"
 * with none).
 *
 * The kernel has no card-change interrupt, so card_check() runs before
 * each request and compares the socket's generation count with the one
 * mounted. A new card is mounted right away; FatFs gives each mount a new
 * id, so handles from the previous card fail with FR_INVALID_OBJECT.
 *
 * In a RAM boot the same code serves Windows CE's FAT partition in the NAND
 * as /NAND, FatFs drive 1 (nanddisk.c). Card paths go to FatFs as they are,
 * on the default drive 0; NAND paths get a "1:" prefix.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "card.h"

#define IO_SECTORS  (GZ_CF_IO_MAX / GZ_CF_SECTOR)

static FATFS fatfs[FF_VOLUMES];
static struct gz_cf_info info;
static bool known;          /* info holds a sample of the socket */
static int status[FF_VOLUMES] = { GZ_ENOENT, GZ_ENOENT };

/* ---- FatFs disk interface (diskio.h): the card, and the NAND -------------- */

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv == NAND_DRIVE)
        return nand_disk_status();
    return pdrv == CARD_DRIVE && info.present ? 0 : STA_NOINIT | STA_NODISK;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv == NAND_DRIVE)
        return nand_disk_read(buff, sector, count);
    if (disk_status(pdrv) & STA_NOINIT)
        return RES_NOTRDY;
    while (count) {
        UINT n = MIN(count, IO_SECTORS);
        if (card_raw_read(sector, n, buff) < 0)
            return RES_ERROR;
        buff += n * GZ_CF_SECTOR;
        sector += n;
        count -= n;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv == NAND_DRIVE)
        return nand_disk_write(buff, sector, count);
    if (disk_status(pdrv) & STA_NOINIT)
        return RES_NOTRDY;
    while (count) {
        UINT n = MIN(count, IO_SECTORS);
        if (card_raw_write(sector, n, buff) < 0)
            return RES_ERROR;
        buff += n * GZ_CF_SECTOR;
        sector += n;
        count -= n;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv == NAND_DRIVE)
        return nand_disk_ioctl(cmd, buff);
    if (disk_status(pdrv) & STA_NOINIT)
        return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:             /* writes are on the card when they return */
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = info.sectors;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

/* ---- mounting --------------------------------------------------------------- */

static int fat_err(FRESULT r)
{
    switch (r) {
    case FR_OK:                 return 0;
    case FR_NO_FILE:
    case FR_NO_PATH:            return GZ_ENOENT;
    case FR_INVALID_NAME:
    case FR_INVALID_PARAMETER:  return GZ_EINVAL;
    case FR_DENIED:             return GZ_ENOSPC;   /* full, or a non-empty dir */
    case FR_EXIST:              return GZ_EEXIST;
    case FR_WRITE_PROTECTED:    return GZ_EPERM;
    case FR_NOT_READY:
    case FR_INVALID_DRIVE:
    case FR_NOT_ENABLED:        return GZ_ENOENT;
    case FR_NO_FILESYSTEM:      return GZ_ECORRUPT;
    case FR_TOO_MANY_OPEN_FILES: return GZ_EFULL;
    case FR_LOCKED:             return GZ_EBUSY;
    case FR_NOT_ENOUGH_CORE:    return GZ_ENOMEM;
    default:                    return GZ_EIO;  /* disk errors, stale handles */
    }
}

void card_check(void)
{
    struct gz_cf_info now;
    char line[96];

    if (card_raw_info(&now) < 0)
        now.present = 0;
    if (known && now.generation == info.generation && now.present == info.present)
        return;
    bool was = known && status[CARD_DRIVE] == 0;
    known = true;
    info = now;
    f_unmount("0:");
    status[CARD_DRIVE] = GZ_ENOENT;
    if (info.present)
        status[CARD_DRIVE] = fat_err(f_mount(&fatfs[CARD_DRIVE], "0:", 1));

    if (status[CARD_DRIVE] == 0) {
        snprintf(line, sizeof(line), "fs: card mounted (%s, %u MB)",
                 info.model, (unsigned)(info.sectors / 2048));
        card_log(line);
    } else if (info.present) {
        snprintf(line, sizeof(line), "fs: card has no FAT volume (%d)", status[CARD_DRIVE]);
        card_log(line);
    } else if (was) {
        card_log("fs: card removed");
    }
}

int card_status(void)
{
    return status[CARD_DRIVE];
}

int card_mount_nand(bool writable)
{
    char line[96];
    int rv = nand_disk_open(writable);

    if (!rv)
        rv = fat_err(f_mount(&fatfs[NAND_DRIVE], "1:", 1));
    status[NAND_DRIVE] = rv;
    if (rv) {
        snprintf(line, sizeof(line), "fs: no FAT volume in the NAND (%d)", rv);
    } else {
        DWORD free_clusters;
        FATFS *fs;
        uint32_t mb = 0;
        if (f_getfree("1:", &free_clusters, &fs) == FR_OK)
            mb = (uint32_t)((uint64_t)(fs->n_fatent - 2) * fs->csize / 2048);
        snprintf(line, sizeof(line), "fs: NAND FAT volume mounted (%u MB, %s)",
                 (unsigned)mb, writable ? "read/write" : "read-only");
    }
    card_log(line);
    return rv;
}

int nand_status(void)
{
    return status[NAND_DRIVE];
}

void card_sync(void)
{
    if (!status[NAND_DRIVE] && nand_disk_flush() < 0)
        card_log("fs: NAND write-back failed");
}

/* Static buffers for "1:" paths: dispatch, an operation and a rename's
 * second path each hold one while a request is served. */
const char *card_path(const char *path)
{
    static char bufs[3][FS_PATH_MAX + 2];
    static unsigned next;
    size_t n = strlen(CARD_PATH);

    if (!strncmp(path, CARD_PATH, n) && (!path[n] || path[n] == '/'))
        return path + n;
    n = strlen(NAND_PATH);
    if (strncmp(path, NAND_PATH, n) || (path[n] && path[n] != '/'))
        return NULL;
    char *buf = bufs[next++ % 3];
    snprintf(buf, sizeof(bufs[0]), "1:%s", path + n);
    return buf;
}

int card_volume(const char *fpath)
{
    return fpath[0] == '1' && fpath[1] == ':' ? NAND_DRIVE : CARD_DRIVE;
}

bool card_is_root(const char *fpath)
{
    if (card_volume(fpath) == NAND_DRIVE)
        fpath += 2;
    return !fpath[0] || !strcmp(fpath, "/");
}

/* ---- operations ------------------------------------------------------------ */

int card_list(const char *path, uint32_t skip, struct fs_dirent *out, int max, bool *more)
{
    FILINFO fno;
    DIR dir;
    int n = 0;
    FRESULT r;

    if (status[card_volume(path)])
        return status[card_volume(path)];
    r = f_opendir(&dir, path);
    if (r)
        return fat_err(r);
    while ((r = f_readdir(&dir, &fno)) == FR_OK && fno.fname[0]) {
        /* Hidden and system files are the OS's (".Trashes", "System Volume
         * Information" and the like), not the user's. */
        if (fno.fattrib & (AM_HID | AM_SYS))
            continue;
        if (skip) {
            skip--;
            continue;
        }
        if (n == max) {
            *more = true;
            break;
        }
        struct fs_dirent *e = &out[n++];
        memset(e, 0, sizeof(*e));
        e->type = (fno.fattrib & AM_DIR) ? FS_DIR : FS_FILE;
        e->size = (fno.fattrib & AM_DIR) ? 0 : (uint32_t)fno.fsize;
        strlcpy(e->name, fno.fname, sizeof(e->name));
        e->name_len = (uint8_t)strlen(e->name);
    }
    f_closedir(&dir);
    return r ? fat_err(r) : n;
}

int card_open(struct card_file *cf, const char *path, uint32_t flags)
{
    BYTE mode = 0;
    FRESULT r;

    if (status[card_volume(path)])
        return status[card_volume(path)];
    if (flags & FS_O_READ)
        mode |= FA_READ;
    if (flags & FS_O_WRITE)
        mode |= FA_WRITE;
    if (!mode)
        mode = FA_READ;
    if ((flags & (FS_O_CREATE | FS_O_EXCL)) == (FS_O_CREATE | FS_O_EXCL))
        mode |= FA_CREATE_NEW;
    else if ((flags & (FS_O_CREATE | FS_O_TRUNC)) == (FS_O_CREATE | FS_O_TRUNC))
        mode |= FA_CREATE_ALWAYS;
    else if (flags & FS_O_CREATE)
        mode |= FA_OPEN_ALWAYS;
    r = f_open(&cf->f, path, mode);
    if (r)
        return fat_err(r);
    /* FatFs truncates only on create; FS_O_TRUNC of an existing file and
     * FS_O_APPEND (every write at the end, as in littlefs) are done here. */
    if ((flags & FS_O_TRUNC) && !(mode & FA_CREATE_ALWAYS) && (mode & FA_WRITE))
        r = f_truncate(&cf->f);
    if (r) {
        f_close(&cf->f);
        return fat_err(r);
    }
    cf->append = (flags & FS_O_APPEND) != 0;
    return 0;
}

int card_read(struct card_file *cf, void *buf, uint32_t len)
{
    UINT n = 0;
    FRESULT r = f_read(&cf->f, buf, len, &n);

    return r ? fat_err(r) : (int)n;
}

int card_write(struct card_file *cf, const void *buf, uint32_t len)
{
    UINT n = 0;
    FRESULT r = FR_OK;

    if (cf->append)
        r = f_lseek(&cf->f, f_size(&cf->f));
    if (!r)
        r = f_write(&cf->f, buf, len, &n);
    if (r)
        return fat_err(r);
    return n || !len ? (int)n : GZ_ENOSPC;
}

int card_set_end(struct card_file *cf)
{
    return fat_err(f_truncate(&cf->f));
}

int card_seek(struct card_file *cf, int32_t off, uint32_t whence)
{
    int64_t base = whence == FS_SEEK_CUR ? (int64_t)f_tell(&cf->f)
                 : whence == FS_SEEK_END ? (int64_t)f_size(&cf->f) : 0;
    int64_t to = base + off;
    FRESULT r;

    if (to < 0 || to > INT32_MAX)
        return GZ_EINVAL;
    r = f_lseek(&cf->f, (FSIZE_t)to);
    return r ? fat_err(r) : (int)f_tell(&cf->f);
}

int card_close(struct card_file *cf)
{
    return fat_err(f_close(&cf->f));
}

int card_stat(const char *path, struct fs_stat *st)
{
    FILINFO fno;
    FRESULT r;

    if (status[card_volume(path)])
        return status[card_volume(path)];
    if (card_is_root(path)) {
        st->type = FS_DIR;
        st->size = 0;
        return 0;
    }
    r = f_stat(path, &fno);
    if (r)
        return fat_err(r);
    st->type = (fno.fattrib & AM_DIR) ? FS_DIR : FS_FILE;
    st->size = (fno.fattrib & AM_DIR) ? 0 : (uint32_t)fno.fsize;
    return 0;
}

int card_mkdir(const char *path)
{
    int st = status[card_volume(path)];

    return st ? st : fat_err(f_mkdir(path));
}

int card_remove(const char *path)
{
    FILINFO fno;
    FRESULT r;

    if (status[card_volume(path)])
        return status[card_volume(path)];
    r = f_unlink(path);
    /* FR_DENIED is a directory with something in it, or a read-only file. */
    if (r == FR_DENIED && f_stat(path, &fno) == FR_OK && (fno.fattrib & AM_DIR))
        return GZ_ENOTEMPTY;
    return fat_err(r);
}

int card_rename(const char *from, const char *to)
{
    FILINFO fno;
    int vol = card_volume(from);

    if (status[vol])
        return status[vol];
    if (card_volume(to) != vol)
        return GZ_EINVAL;
    /* littlefs replaces an existing file (POSIX rename); FatFs refuses. */
    if (f_stat(to, &fno) == FR_OK) {
        if (fno.fattrib & AM_DIR)
            return GZ_EISDIR;
        /* Short names are case-insensitive: "a.txt" to "A.txt" is the
         * same entry, renamed in place. */
        if (strcasecmp(from, to)) {
            FRESULT r = f_unlink(to);
            if (r)
                return fat_err(r);
        }
    }
    /* FatFs takes the new name without a drive. */
    if (vol == NAND_DRIVE)
        to += 2;
    return fat_err(f_rename(from, to));
}

int card_statfs(const char *path, struct fs_info *out)
{
    DWORD free_clusters;
    FATFS *fs;
    int vol = card_volume(path);

    memset(out, 0, sizeof(*out));
    out->mounted = status[vol];
    if (status[vol])
        return 0;
    FRESULT r = f_getfree(vol == NAND_DRIVE ? "1:" : "0:", &free_clusters, &fs);
    if (r)
        return fat_err(r);
    uint32_t cluster = (uint32_t)fs->csize * GZ_CF_SECTOR;
    out->total = (uint32_t)MIN((uint64_t)(fs->n_fatent - 2) * cluster, UINT32_MAX);
    out->used = out->total - (uint32_t)MIN((uint64_t)free_clusters * cluster, out->total);
    return 0;
}
