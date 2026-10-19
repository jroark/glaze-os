/* File system client: requests to the file server (see glaze/fs.h). */

#include <glaze/fs.h>
#include <glaze/sys.h>
#include <glaze/thread.h>
#include <string.h>

static int port, session = -1;
static uint8_t *buf;

/* One call at a time per process: the shared buffer is the session's. */
static struct gz_mutex session_lock = GZ_MUTEX_INIT;

static void release(struct gz_mutex **m)
{
    gz_mutex_unlock(*m);
}

#define SESSION_LOCKED() \
    struct gz_mutex *held_ __attribute__((cleanup(release))) = (gz_mutex_lock(&session_lock), &session_lock)

/* Connects on first use. The file server mounts the flash before it
 * registers its port, so wait a little for it after boot. */
static int connect(void)
{
    struct gz_msg m, r;

    if (session >= 0)
        return 0;
    for (int tries = 0; (port = gz_port_lookup(FS_PORT_NAME)) <= 0; tries++) {
        if (tries == 100)
            return GZ_ENOENT;
        gz_sleep_ms(50);
    }
    memset(&m, 0, sizeof(m));
    m.w[0] = FS_HELLO;
    m.w[1] = (uint32_t)gz_getpid();
    if (gz_send(port, &m, &r) < 0 || (int)r.w[0] < 0)
        return r.w[0] ? (int)r.w[0] : GZ_EIO;
    buf = gz_shm_map((int)r.w[2]);
    if (!buf)
        return GZ_ENOMEM;
    session = (int)r.w[1];
    return 0;
}

static int call(uint32_t op, uint32_t a, uint32_t b, uint32_t c, struct gz_msg *reply)
{
    struct gz_msg m, r;

    memset(&m, 0, sizeof(m));
    m.w[0] = op;
    m.w[1] = (uint32_t)session;
    m.w[2] = a;
    m.w[3] = b;
    m.w[4] = c;
    if (gz_send(port, &m, &r) < 0)
        return GZ_EIO;
    if (reply)
        *reply = r;
    return (int)r.w[0];
}

static int put_path(const char *path, uint32_t at)
{
    size_t n = strlen(path);

    if (n >= FS_PATH_MAX)
        return GZ_EINVAL;
    memcpy(buf + at, path, n + 1);
    return 0;
}

static int path_call(uint32_t op, const char *path, uint32_t a)
{
    int rv = connect();

    if (!rv)
        rv = put_path(path, 0);
    return rv ? rv : call(op, a, 0, 0, NULL);
}

int fs_open(const char *path, int flags)
{
    SESSION_LOCKED();
    return path_call(FS_OPEN, path, (uint32_t)flags);
}

int fs_read(int fd, void *dst, uint32_t len)
{
    SESSION_LOCKED();
    uint32_t done = 0;
    int rv = connect();

    if (rv)
        return rv;
    while (done < len) {
        uint32_t n = MIN(len - done, FS_IO_MAX);
        rv = call(FS_READ, (uint32_t)fd, n, 0, NULL);
        if (rv < 0)
            return done ? (int)done : rv;
        memcpy((uint8_t *)dst + done, buf, (size_t)rv);
        done += (uint32_t)rv;
        if ((uint32_t)rv < n)
            break;
    }
    return (int)done;
}

int fs_write(int fd, const void *src, uint32_t len)
{
    SESSION_LOCKED();
    uint32_t done = 0;
    int rv = connect();

    if (rv)
        return rv;
    while (done < len) {
        uint32_t n = MIN(len - done, FS_IO_MAX);
        memcpy(buf, (const uint8_t *)src + done, n);
        rv = call(FS_WRITE, (uint32_t)fd, n, 0, NULL);
        if (rv < 0)
            return rv;
        done += (uint32_t)rv;
    }
    return (int)done;
}

int fs_seek(int fd, int32_t offset, int whence)
{
    SESSION_LOCKED();
    int rv = connect();

    return rv ? rv : call(FS_SEEK, (uint32_t)fd, (uint32_t)offset, (uint32_t)whence, NULL);
}

int fs_close(int fd)
{
    SESSION_LOCKED();
    int rv = connect();

    return rv ? rv : call(FS_CLOSE, (uint32_t)fd, 0, 0, NULL);
}

int fs_stat(const char *path, struct fs_stat *st)
{
    SESSION_LOCKED();
    int rv = path_call(FS_STAT, path, 0);

    if (rv >= 0 && st)
        memcpy(st, buf, sizeof(*st));
    return rv;
}

int fs_list(const char *path, uint32_t skip, struct fs_dirent *out, int max)
{
    SESSION_LOCKED();
    int total = 0, rv = connect();
    struct gz_msg r;

    if (!rv)
        rv = put_path(path, 0);
    while (!rv && total < max) {
        rv = call(FS_LIST, skip + (uint32_t)total, 0, 0, &r);
        if (rv < 0)
            return total ? total : rv;
        int n = MIN(rv, max - total);
        memcpy(out + total, buf, (size_t)n * sizeof(*out));
        total += n;
        if (!r.w[1])
            break;              /* no more entries */
        rv = put_path(path, 0);
    }
    return rv < 0 ? rv : total;
}

int fs_mkdir(const char *path)
{
    SESSION_LOCKED();
    return path_call(FS_MKDIR, path, 0);
}

int fs_mkdirs(const char *path)
{
    char part[FS_PATH_MAX];
    size_t n = strlen(path);

    if (n >= sizeof(part))
        return GZ_EINVAL;
    for (size_t i = 1; i <= n; i++) {
        if (path[i] != '/' && path[i] != 0)
            continue;
        memcpy(part, path, i);
        part[i] = 0;
        int rv = fs_mkdir(part);
        if (rv < 0 && rv != GZ_EEXIST)
            return rv;
    }
    return 0;
}

int fs_remove(const char *path)
{
    SESSION_LOCKED();
    return path_call(FS_REMOVE, path, 0);
}

int fs_rename(const char *from, const char *to)
{
    SESSION_LOCKED();
    int rv = connect();

    if (!rv)
        rv = put_path(from, 0);
    if (!rv)
        rv = put_path(to, FS_PATH_MAX);
    return rv ? rv : call(FS_RENAME, 0, 0, 0, NULL);
}

int fs_statfs(struct fs_info *info)
{
    SESSION_LOCKED();
    int rv = connect();

    if (!rv)
        rv = call(FS_STATFS, 0, 0, 0, NULL);
    if (rv >= 0)
        memcpy(info, buf, sizeof(*info));
    return rv;
}

int fs_statvfs(const char *path, struct fs_info *info)
{
    SESSION_LOCKED();
    int rv = connect();

    if (!rv)
        rv = put_path(path, 0);
    if (!rv)
        rv = call(FS_STATFS, 1, 0, 0, NULL);
    if (rv >= 0)
        memcpy(info, buf, sizeof(*info));
    return rv;
}

int fs_format(void)
{
    SESSION_LOCKED();
    int rv = connect();

    return rv ? rv : call(FS_FORMAT, 0, 0, 0, NULL);
}

int fs_read_file(const char *path, void *dst, uint32_t max)
{
    SESSION_LOCKED();
    int fd = fs_open(path, FS_O_READ);

    if (fd < 0)
        return fd;
    int n = fs_read(fd, dst, max);
    fs_close(fd);
    return n;
}

int fs_write_file(const char *path, const void *src, uint32_t len)
{
    SESSION_LOCKED();
    int fd = fs_open(path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);

    if (fd < 0)
        return fd;
    int n = fs_write(fd, src, len);
    int rv = fs_close(fd);
    return n < 0 ? n : rv < 0 ? rv : n;
}

const char *fs_strerror(int err)
{
    switch (err) {
    case GZ_ENOENT:     return "No such file or folder";
    case GZ_EEXIST:     return "An item with that name already exists";
    case GZ_ENOTEMPTY:  return "The folder isn't empty";
    case GZ_ENOSPC:     return "Storage is full";
    case GZ_EISDIR:     return "That is a folder";
    case GZ_ENOTDIR:    return "That isn't a folder";
    case GZ_ECORRUPT:   return "The data is damaged";
    case GZ_EINVAL:     return "Invalid name";
    case GZ_ENOMEM:     return "Out of memory";
    default:            return "Storage error";
    }
}
