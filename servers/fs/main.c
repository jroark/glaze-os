/*
 * File server: littlefs on Dhara on the flash region the kernel grants
 * (SYS_FLASH_*), served over IPC to apps (see glaze/fs.h and ftl.c), plus
 * a CompactFlash card's FAT volume as /Card (card.c).
 *
 * In a RAM boot (glaze/bootinfo.h) the region is a RAM disk the kernel
 * sizes (SYS_FLASH_INFO), and Windows CE's FAT volume in the NAND is
 * /NAND (nanddisk.c on SYS_NAND_*).
 */

#include <glaze/fs.h>
#include <glaze/sys.h>
#include <stdio.h>
#include <string.h>
#include "card.h"
#include "folders.h"
#include "ftl.h"

#define MAX_SESSIONS    16
#define MAX_FILES       32

struct session {
    int pid;                /* 0 = free */
    int shm;
    uint8_t *buf;
};

struct file {
    bool used;
    bool card;              /* on /Card, else littlefs */
    int session;
    union {
        lfs_file_t f;
        struct card_file cf;
    };
};

static lfs_t lfs;
static int mount_err;       /* 0 when mounted */
static struct session sessions[MAX_SESSIONS];
static struct file files[MAX_FILES];

/* ---- platform hooks for ftl.c -------------------------------------------- */

static bool flash_badmap;
int flash_raw_is_bad(uint32_t block) { return flash_badmap ? gz_flash_bad(block) : 0; }

int flash_raw_read(uint32_t page, uint8_t *buf)
{
    return gz_flash_read(page, buf);
}

int flash_raw_prog(uint32_t page, const uint8_t *buf)
{
    return gz_flash_prog(page, buf);
}

int flash_raw_erase(uint32_t block)
{
    return gz_flash_erase(block);
}

void ftl_log(const char *msg)
{
    char line[96];

    snprintf(line, sizeof(line), "%s\n", msg);
    gz_log(line, (uint32_t)strlen(line));
}

/* ---- platform hooks for card.c and FatFs ---------------------------------- */

int card_raw_info(struct gz_cf_info *info)
{
    return gz_cf_info(info);
}

int card_raw_read(uint32_t lba, uint32_t count, void *buf)
{
    return gz_cf_read(lba, count, buf);
}

int card_raw_write(uint32_t lba, uint32_t count, const void *buf)
{
    return gz_cf_write(lba, count, buf);
}

int nand_raw_read(uint32_t page, void *buf)
{
    return gz_nand_read(page, buf);
}

int nand_raw_prog(uint32_t page, const void *buf)
{
    return gz_nand_prog(page, buf);
}

int nand_raw_erase(uint32_t block)
{
    return gz_nand_erase(block);
}

void card_log(const char *msg)
{
    ftl_log(msg);
}

/* FAT timestamps: local time, two-second resolution, from 1980. */
DWORD get_fattime(void)
{
    struct gz_datetime t;

    if (gz_rtc_get(&t) < 0 || t.year < 1980)
        return (DWORD)(2026 - 1980) << 25 | 1u << 21 | 1u << 16;
    return (DWORD)(t.year - 1980) << 25 | (DWORD)t.month << 21 | (DWORD)t.day << 16 |
           (DWORD)t.hour << 11 | (DWORD)t.minute << 5 | t.second / 2u;
}

/* ---- helpers --------------------------------------------------------------- */

static int gz_err(int e)
{
    switch (e) {
    case LFS_ERR_NOENT:         return GZ_ENOENT;
    case LFS_ERR_EXIST:         return GZ_EEXIST;
    case LFS_ERR_ISDIR:         return GZ_EISDIR;
    case LFS_ERR_NOTDIR:        return GZ_ENOTDIR;
    case LFS_ERR_NOTEMPTY:      return GZ_ENOTEMPTY;
    case LFS_ERR_NOSPC:
    case LFS_ERR_FBIG:          return GZ_ENOSPC;
    case LFS_ERR_CORRUPT:       return GZ_ECORRUPT;
    case LFS_ERR_INVAL:
    case LFS_ERR_NAMETOOLONG:   return GZ_EINVAL;
    case LFS_ERR_NOMEM:         return GZ_ENOMEM;
    default:                    return e < 0 ? GZ_EIO : e;
    }
}

/* The NUL-terminated path at `at` in the session buffer, or NULL. */
static const char *path_arg(struct session *s, uint32_t at)
{
    char *p = (char *)s->buf + at;

    if (!memchr(p, 0, FS_PATH_MAX))
        return NULL;
    return p;
}

static struct file *file_arg(int sess, uint32_t h)
{
    if (h >= MAX_FILES || !files[h].used || files[h].session != sess)
        return NULL;
    return &files[h];
}

static void close_session_files(int sess)
{
    for (int i = 0; i < MAX_FILES; i++) {
        if (files[i].used && files[i].session == sess) {
            if (files[i].card)
                card_close(&files[i].cf);
            else
                lfs_file_close(&lfs, &files[i].f);
            files[i].used = false;
        }
    }
}

/* ---- operations ------------------------------------------------------------ */

static int op_hello(int pid, struct gz_msg *r)
{
    int pick = -1;

    /* Reuse this process's session, else a free one, else a dead one's. */
    for (int i = 0; i < MAX_SESSIONS && pick < 0; i++)
        if (sessions[i].pid == pid)
            pick = i;
    for (int i = 0; i < MAX_SESSIONS && pick < 0; i++)
        if (!sessions[i].pid || !gz_proc_alive(sessions[i].pid))
            pick = i;
    if (pick < 0)
        return GZ_EFULL;
    struct session *s = &sessions[pick];
    close_session_files(pick);
    if (!s->buf) {
        s->shm = gz_shm_create(FS_BUF_SIZE);
        s->buf = s->shm > 0 ? gz_shm_map(s->shm) : NULL;
        if (!s->buf)
            return GZ_ENOMEM;
    }
    s->pid = pid;
    r->w[1] = (uint32_t)pick;
    r->w[2] = (uint32_t)s->shm;
    return 0;
}

static int op_list(struct session *s, const char *path, uint32_t skip, struct gz_msg *r)
{
    struct fs_dirent *out = (struct fs_dirent *)s->buf;
    const int max = (int)(FS_BUF_SIZE / sizeof(*out));
    struct lfs_info info;
    lfs_dir_t dir;
    int n = 0, rv;
    const char *cpath = card_path(path);
    bool root = !strcmp(path, "/") || !path[0];

    if (cpath) {
        bool more = false;
        rv = card_list(cpath, skip, out, max, &more);
        r->w[1] = more;
        return rv;
    }
    if (mount_err)
        return mount_err;
    rv = lfs_dir_open(&lfs, &dir, path);
    if (rv)
        return gz_err(rv);
    /* A mounted card, then a mounted NAND volume, are the root's first
     * entries (written only now: the path shares the buffer with the
     * entries). */
    const char *mounts[2] = { card_status() ? NULL : CARD_NAME, nand_status() ? NULL : NAND_NAME };
    for (int i = 0; root && i < 2; i++) {
        if (!mounts[i])
            continue;
        if (skip) {
            skip--;
            continue;
        }
        memset(&out[n], 0, sizeof(out[n]));
        out[n].type = FS_DIR;
        strlcpy(out[n].name, mounts[i], sizeof(out[n].name));
        out[n].name_len = (uint8_t)strlen(out[n].name);
        n++;
    }
    while ((rv = lfs_dir_read(&lfs, &dir, &info)) > 0) {
        if (!strcmp(info.name, ".") || !strcmp(info.name, ".."))
            continue;
        if (root && (!strcmp(info.name, CARD_NAME) || !strcmp(info.name, NAND_NAME)))
            continue;           /* never made by this server; see op_mkdir */
        if (skip) {
            skip--;
            continue;
        }
        if (n == max) {
            r->w[1] = 1;        /* more to come */
            break;
        }
        struct fs_dirent *e = &out[n++];
        memset(e, 0, sizeof(*e));
        e->type = info.type == LFS_TYPE_DIR ? FS_DIR : FS_FILE;
        e->size = info.type == LFS_TYPE_DIR ? 0 : info.size;
        strlcpy(e->name, info.name, sizeof(e->name));
        e->name_len = (uint8_t)strlen(e->name);
    }
    lfs_dir_close(&lfs, &dir);
    return rv < 0 ? gz_err(rv) : n;
}

static int op_open(int sess, const char *path, uint32_t flags)
{
    int lflags = 0, h;

    for (h = 0; h < MAX_FILES && files[h].used; h++)
        ;
    if (h == MAX_FILES)
        return GZ_EFULL;
    const char *cpath = card_path(path);
    if (cpath) {
        int rv = card_open(&files[h].cf, cpath, flags);
        if (rv)
            return rv;
        files[h].used = true;
        files[h].card = true;
        files[h].session = sess;
        return h;
    }
    if (mount_err)
        return mount_err;
    if ((flags & (FS_O_READ | FS_O_WRITE)) == (FS_O_READ | FS_O_WRITE))
        lflags = LFS_O_RDWR;
    else
        lflags = (flags & FS_O_WRITE) ? LFS_O_WRONLY : LFS_O_RDONLY;
    if (flags & FS_O_CREATE)
        lflags |= LFS_O_CREAT;
    if (flags & FS_O_TRUNC)
        lflags |= LFS_O_TRUNC;
    if (flags & FS_O_EXCL)
        lflags |= LFS_O_EXCL;
    if (flags & FS_O_APPEND)
        lflags |= LFS_O_APPEND;
    int rv = lfs_file_open(&lfs, &files[h].f, path, lflags);
    if (rv)
        return gz_err(rv);
    files[h].used = true;
    files[h].card = false;
    files[h].session = sess;
    return h;
}

static int op_stat(struct session *s, const char *path)
{
    struct lfs_info info;
    struct fs_stat st;
    const char *cpath = card_path(path);
    int rv;

    if (cpath) {
        rv = card_stat(cpath, &st);
        if (!rv)
            memcpy(s->buf, &st, sizeof(st));
        return rv;
    }
    if (mount_err)
        return mount_err;
    rv = lfs_stat(&lfs, path, &info);
    if (rv)
        return gz_err(rv);
    st.type = info.type == LFS_TYPE_DIR ? FS_DIR : FS_FILE;
    st.size = info.type == LFS_TYPE_DIR ? 0 : info.size;
    memcpy(s->buf, &st, sizeof(st));
    return 0;
}

static int op_statfs(struct session *s, uint32_t by_path)
{
    struct ftl_stats st;
    struct fs_info info;
    const char *path = by_path ? path_arg(s, 0) : "/";

    if (!path)
        return GZ_EINVAL;
    const char *cpath = card_path(path);
    if (cpath) {
        int rv = card_statfs(cpath, &info);
        memcpy(s->buf, &info, sizeof(info));
        return rv;
    }

    ftl_get_stats(&st);
    memset(&info, 0, sizeof(info));
    info.mounted = mount_err;
    info.total = st.capacity;
    if (!mount_err) {
        lfs_ssize_t used = lfs_fs_size(&lfs);
        info.used = used > 0 ? (uint32_t)used * lfs.cfg->block_size : 0;
    }
    info.flash_blocks = st.blocks;
    info.bad_blocks = st.bad_blocks;
    info.ecc_corrected = st.corrected;
    info.ecc_failed = st.uncorrectable;
    memcpy(s->buf, &info, sizeof(info));
    return 0;
}

static void make_folders(void);

static int op_format(void)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
        close_session_files(i);
    if (!mount_err)
        lfs_unmount(&lfs);
    mount_err = gz_err(ftl_format(&lfs));
    if (!mount_err)
        make_folders();
    return mount_err;
}

/* Read-only access for recovering CE exports/CCFS metadata. The existing
 * kernel syscall enforces RAM boot, nand=off, logical mapping and ECC. */
static int op_nand_read(struct session *s,uint32_t offset,uint32_t len)
{
    uint8_t page[512] __attribute__((aligned(4)));
    if(offset>16u*1024u*1024u || len>FS_IO_MAX || len>16u*1024u*1024u-offset)return GZ_EINVAL;
    uint32_t done=0;
    while(done<len){
        uint32_t at=offset+done,n=MIN(len-done,512u-at%512u);
        int rv=gz_nand_read(at/512u,page);if(rv<0)return rv;
        memcpy(s->buf+done,page+at%512u,n);done+=n;
    }
    return (int)done;
}

static int dispatch(const struct gz_msg *m, struct gz_msg *r)
{
    uint32_t op = m->w[0];
    int sess = (int)m->w[1];

    if (op == FS_HELLO)
        return op_hello((int)m->w[1], r);
    if (sess < 0 || sess >= MAX_SESSIONS || !sessions[sess].pid)
        return GZ_EPERM;
    struct session *s = &sessions[sess];
    card_check();
    if (op == FS_NAND_READ) return op_nand_read(s,m->w[2],m->w[3]);
    if (op == FS_STATFS)
        return op_statfs(s, m->w[2]);
    if (op == FS_FORMAT)
        return op_format();
    if (op == FS_CARD_FORMAT) {
        for (int i=0;i<MAX_FILES;i++) {
            if (files[i].used && files[i].card && files[i].cf.f.obj.fs &&
                files[i].cf.f.obj.fs->pdrv==CARD_DRIVE) {
                /* Mount ids invalidate any old handles after format. */
                card_close(&files[i].cf); files[i].used=false;
            }
        }
        return card_format_recovery(m->w[2],m->w[3]);
    }

    const char *path = path_arg(s, 0);
    const char *cpath = path ? card_path(path) : NULL;
    struct file *f = file_arg(sess, m->w[2]);
    int rv;

    /* Card files and paths go to card.c; the rest needs the flash volume. */
    switch (op) {
    case FS_STAT:
        return path ? op_stat(s, path) : GZ_EINVAL;
    case FS_LIST:
        return path ? op_list(s, path, m->w[2], r) : GZ_EINVAL;
    case FS_OPEN:
        return path ? op_open(sess, path, m->w[2]) : GZ_EINVAL;
    case FS_READ:
        if (!f)
            return GZ_EINVAL;
        if (f->card)
            return card_read(&f->cf, s->buf, MIN(m->w[3], FS_IO_MAX));
        rv = lfs_file_read(&lfs, &f->f, s->buf, MIN(m->w[3], FS_IO_MAX));
        return gz_err(rv);
    case FS_WRITE:
        if (!f)
            return GZ_EINVAL;
        if (f->card)
            return card_write(&f->cf, s->buf, MIN(m->w[3], FS_IO_MAX));
        rv = lfs_file_write(&lfs, &f->f, s->buf, MIN(m->w[3], FS_IO_MAX));
        return gz_err(rv);
    case FS_SEEK: {
        static const int whence[] = { LFS_SEEK_SET, LFS_SEEK_CUR, LFS_SEEK_END };
        if (!f || m->w[4] > FS_SEEK_END)
            return GZ_EINVAL;
        if (f->card)
            return card_seek(&f->cf, (int32_t)m->w[3], m->w[4]);
        return gz_err(lfs_file_seek(&lfs, &f->f, (lfs_soff_t)m->w[3], whence[m->w[4]]));
    }
    case FS_CLOSE:
        if (!f)
            return GZ_EINVAL;
        rv = f->card ? card_close(&f->cf) : gz_err(lfs_file_close(&lfs, &f->f));
        f->used = false;
        return rv;
    case FS_SET_END:
        if (!f)
            return GZ_EINVAL;
        if (f->card)
            return card_set_end(&f->cf);
        if (!(f->f.flags & LFS_O_WRONLY))
            return GZ_EPERM;
        rv = lfs_file_tell(&lfs, &f->f);
        return rv < 0 ? gz_err(rv) : gz_err(lfs_file_truncate(&lfs, &f->f, (lfs_size_t)rv));
    case FS_MKDIR:
    case FS_REMOVE:
        if (!path)
            return GZ_EINVAL;
        /* /Card and /NAND are mount points, mounted or not. */
        if (cpath && card_is_root(cpath))
            return op == FS_MKDIR ? GZ_EEXIST : GZ_EPERM;
        if (cpath)
            return op == FS_MKDIR ? card_mkdir(cpath) : card_remove(cpath);
        if (mount_err)
            return mount_err;
        return gz_err(op == FS_MKDIR ? lfs_mkdir(&lfs, path) : lfs_remove(&lfs, path));
    case FS_RENAME: {
        const char *to = path_arg(s, FS_PATH_MAX);
        if (!path || !to)
            return GZ_EINVAL;
        const char *cto = card_path(to);
        if ((cpath && card_is_root(cpath)) || (cto && card_is_root(cto)))
            return GZ_EPERM;
        if (!cpath != !cto || (cpath && card_volume(cpath) != card_volume(cto)))
            return GZ_EINVAL;   /* no moves between volumes */
        if (cpath)
            return card_rename(cpath, cto);
        if (mount_err)
            return mount_err;
        return gz_err(lfs_rename(&lfs, path, to));
    }
    default:
        return GZ_EINVAL;
    }
}

/* ---- new volumes ----------------------------------------------------------- */

/* A blank or erased volume gets the standard folders and nothing else. The
 * factory volume, with sample documents and pictures, is built into the
 * NAND image by host/mkfsimg. */
static void make_folders(void)
{
    for (unsigned i = 0; i < sizeof(fs_standard_folders) / sizeof(fs_standard_folders[0]); i++)
        lfs_mkdir(&lfs, fs_standard_folders[i]);
}

int main(void)
{
    struct gz_msg m, r;
    struct gz_flash_info fi = { .blocks = GZ_FLASH_BLOCKS };
    uint32_t t0 = gz_time_ms();
    char line[96];

    gz_flash_info(&fi);
    flash_badmap = (fi.flags & GZ_FLASH_BADMAP) != 0;
    int rv = ftl_mount(&lfs, fi.blocks);
    mount_err = rv < 0 ? gz_err(rv) : 0;
    if (rv == 1)
        make_folders();
    snprintf(line, sizeof(line), "fs: %s %s in %u ms\n",
             rv < 0 ? "mount failed" : rv ? "formatted" : "mounted",
             fi.flags & GZ_FLASH_RAM ? "RAM disk" : "flash", gz_time_ms() - t0);
    gz_log(line, (uint32_t)strlen(line));
    if (fi.flags & GZ_FLASH_NAND)
        card_mount_nand((fi.flags & GZ_FLASH_NAND_RW) != 0);

    int port = gz_port_create();
    if (port < 0 || gz_port_register(port, FS_PORT_NAME) < 0)
        return 1;
    for (;;) {
        int token = gz_recv(port, &m, -1);
        if (token <= 0)
            continue;
        memset(&r, 0, sizeof(r));
        r.w[0] = (uint32_t)dispatch(&m, &r);
        card_sync();            /* NAND writes are on the flash before the reply */
        gz_reply(token, &r);
    }
}
