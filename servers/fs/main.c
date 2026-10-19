/*
 * File server: littlefs on Dhara on the flash region the kernel grants
 * (SYS_FLASH_*), served over IPC to apps (see glaze/fs.h and ftl.c).
 */

#include <glaze/fs.h>
#include <glaze/sys.h>
#include <stdio.h>
#include <string.h>
#include "ftl.h"

#define MAX_SESSIONS    8
#define MAX_FILES       16

struct session {
    int pid;                /* 0 = free */
    int shm;
    uint8_t *buf;
};

struct file {
    bool used;
    int session;
    lfs_file_t f;
};

static lfs_t lfs;
static int mount_err;       /* 0 when mounted */
static struct session sessions[MAX_SESSIONS];
static struct file files[MAX_FILES];

/* ---- platform hooks for ftl.c -------------------------------------------- */

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

    rv = lfs_dir_open(&lfs, &dir, path);
    if (rv)
        return gz_err(rv);
    while ((rv = lfs_dir_read(&lfs, &dir, &info)) > 0) {
        if (!strcmp(info.name, ".") || !strcmp(info.name, ".."))
            continue;
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
    files[h].session = sess;
    return h;
}

static int op_stat(struct session *s, const char *path)
{
    struct lfs_info info;
    struct fs_stat st;
    int rv = lfs_stat(&lfs, path, &info);

    if (rv)
        return gz_err(rv);
    st.type = info.type == LFS_TYPE_DIR ? FS_DIR : FS_FILE;
    st.size = info.type == LFS_TYPE_DIR ? 0 : info.size;
    memcpy(s->buf, &st, sizeof(st));
    return 0;
}

static int op_statfs(struct session *s)
{
    struct ftl_stats st;
    struct fs_info info;

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

static void seed(void);

static int op_format(void)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
        close_session_files(i);
    if (!mount_err)
        lfs_unmount(&lfs);
    mount_err = gz_err(ftl_format(&lfs));
    if (!mount_err)
        seed();
    return mount_err;
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
    if (op == FS_STATFS)
        return op_statfs(s);
    if (op == FS_FORMAT)
        return op_format();
    if (mount_err)
        return mount_err;

    const char *path = path_arg(s, 0);
    struct file *f = file_arg(sess, m->w[2]);
    int rv;

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
        rv = lfs_file_read(&lfs, &f->f, s->buf, MIN(m->w[3], FS_IO_MAX));
        return gz_err(rv);
    case FS_WRITE:
        if (!f)
            return GZ_EINVAL;
        rv = lfs_file_write(&lfs, &f->f, s->buf, MIN(m->w[3], FS_IO_MAX));
        return gz_err(rv);
    case FS_SEEK: {
        static const int whence[] = { LFS_SEEK_SET, LFS_SEEK_CUR, LFS_SEEK_END };
        if (!f || m->w[4] > FS_SEEK_END)
            return GZ_EINVAL;
        return gz_err(lfs_file_seek(&lfs, &f->f, (lfs_soff_t)m->w[3], whence[m->w[4]]));
    }
    case FS_CLOSE:
        if (!f)
            return GZ_EINVAL;
        rv = lfs_file_close(&lfs, &f->f);
        f->used = false;
        return gz_err(rv);
    case FS_MKDIR:
        return path ? gz_err(lfs_mkdir(&lfs, path)) : GZ_EINVAL;
    case FS_REMOVE:
        return path ? gz_err(lfs_remove(&lfs, path)) : GZ_EINVAL;
    case FS_RENAME: {
        const char *to = path_arg(s, FS_PATH_MAX);
        return path && to ? gz_err(lfs_rename(&lfs, path, to)) : GZ_EINVAL;
    }
    default:
        return GZ_EINVAL;
    }
}

/* ---- first boot ------------------------------------------------------------ */

static const char welcome[] =
    "Welcome to Glaze.\n"
    "\n"
    "This folder lives on the BE-300's flash. Files keeps your documents "
    "and pictures here, and they stay put when you turn the device off.\n";

static void write_file(const char *path, const void *data, uint32_t len)
{
    lfs_file_t f;

    if (lfs_file_open(&lfs, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC))
        return;
    lfs_file_write(&lfs, &f, data, len);
    lfs_file_close(&lfs, &f);
}

/* Standard folders, a note and two of the stock photos. */
static void seed(void)
{
    static const char *const photos[][2] = {
        { "res/photo_1.gzi", "/Pictures/Photo 1.gzi" },
        { "res/photo_4.gzi", "/Pictures/Photo 4.gzi" },
    };

    lfs_mkdir(&lfs, "/Documents");
    lfs_mkdir(&lfs, "/Pictures");
    write_file("/Documents/Welcome.txt", welcome, sizeof(welcome) - 1);
    for (unsigned i = 0; i < sizeof(photos) / sizeof(photos[0]); i++) {
        uint32_t size;
        const void *data = gz_romfs_map(photos[i][0], &size);
        if (data)
            write_file(photos[i][1], data, size);
    }
}

int main(void)
{
    struct gz_msg m, r;
    uint32_t t0 = gz_time_ms();
    int rv = ftl_mount(&lfs, GZ_FLASH_BLOCKS);
    char line[96];

    mount_err = rv < 0 ? gz_err(rv) : 0;
    if (rv == 1)
        seed();
    snprintf(line, sizeof(line), "fs: %s in %u ms\n",
             rv < 0 ? "mount failed" : rv ? "formatted" : "mounted", gz_time_ms() - t0);
    gz_log(line, (uint32_t)strlen(line));

    int port = gz_port_create();
    if (port < 0 || gz_port_register(port, FS_PORT_NAME) < 0)
        return 1;
    for (;;) {
        int token = gz_recv(port, &m, -1);
        if (token <= 0)
            continue;
        memset(&r, 0, sizeof(r));
        r.w[0] = (uint32_t)dispatch(&m, &r);
        gz_reply(token, &r);
    }
}
