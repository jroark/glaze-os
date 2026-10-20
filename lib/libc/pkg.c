/* App packages: reading, installing and removing them (see glaze/pkg.h). */

#include <glaze/fs.h>
#include <glaze/pkg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_ENTRIES     64
#define INFO_MAX        512
#define MAX_LISTED      32

struct entry {
    char name[48];
    uint32_t offset;
    uint32_t size;
};

static bool id_ok(const char *id)
{
    if (!*id || strlen(id) > PKG_ID_MAX || *id == '.' || *id == '-')
        return false;
    for (const char *c = id; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '.' || *c == '-'))
            return false;
    }
    return !strstr(id, "..");
}

/* A relative path of plain components: no "..", no empty parts. */
static bool entry_name_ok(const char *name)
{
    if (!*name || *name == '/' || strstr(name, "//") || name[strlen(name) - 1] == '/')
        return false;
    for (const char *p = name; *p; p++) {
        if (*p < 0x21 || *p > 0x7e || *p == '\\')
            return false;
        if (*p == '.' && (p == name || p[-1] == '/') && (p[1] == '.' || p[1] == '/' || !p[1]))
            return false;
    }
    return true;
}

static void copy_field(char *dst, size_t n, const char *src, size_t len)
{
    len = MIN(len, n - 1);
    memcpy(dst, src, len);
    dst[len] = 0;
}

static int parse_info(const char *text, size_t len, struct pkg_info *info)
{
    memset(info, 0, sizeof(*info));
    strlcpy(info->exec, "app", sizeof(info->exec));
    for (size_t i = 0; i < len;) {
        size_t end = i;
        while (end < len && text[end] != '\n')
            end++;
        const char *line = text + i, *eq = memchr(line, '=', end - i);
        size_t vlen = end - i - (eq ? (size_t)(eq + 1 - line) : 0);
        if (vlen && eq && eq[vlen] == '\r')
            vlen--;
        if (eq) {
            size_t klen = (size_t)(eq - line);
            if (klen == 2 && !memcmp(line, "id", 2))
                copy_field(info->id, sizeof(info->id), eq + 1, vlen);
            else if (klen == 4 && !memcmp(line, "name", 4))
                copy_field(info->name, sizeof(info->name), eq + 1, vlen);
            else if (klen == 7 && !memcmp(line, "version", 7))
                copy_field(info->version, sizeof(info->version), eq + 1, vlen);
            else if (klen == 4 && !memcmp(line, "exec", 4))
                copy_field(info->exec, sizeof(info->exec), eq + 1, vlen);
        }
        i = end + 1;
    }
    if (!id_ok(info->id) || !info->name[0] || !entry_name_ok(info->exec) || strchr(info->exec, '/'))
        return GZ_EINVAL;
    return 0;
}

/* Reads and checks a package's table. */
static int read_table(int fd, struct entry *ents, uint32_t *count, uint32_t *file_size)
{
    uint8_t hdr[8];
    int end = fs_seek(fd, 0, FS_SEEK_END);

    if (end < 0)
        return end;
    *file_size = (uint32_t)end;
    if (fs_seek(fd, 0, FS_SEEK_SET) < 0 || fs_read(fd, hdr, 8) != 8 || memcmp(hdr, "GZPK", 4))
        return GZ_EINVAL;
    memcpy(count, hdr + 4, 4);
    if (*count == 0 || *count > MAX_ENTRIES)
        return GZ_EINVAL;
    uint32_t len = *count * (uint32_t)sizeof(struct entry);
    if (fs_read(fd, ents, len) != (int)len)
        return GZ_EINVAL;
    for (uint32_t i = 0; i < *count; i++) {
        struct entry *e = &ents[i];
        if (memchr(e->name, 0, sizeof(e->name)) == NULL || !entry_name_ok(e->name) ||
            !strcmp(e->name, "data") || !strncmp(e->name, "data/", 5) ||
            !strcmp(e->name, ".pkg-ready") || !strncmp(e->name, ".pkg-ready/", 11) ||
            e->offset < 8 + len || e->offset > *file_size || e->size > *file_size - e->offset)
            return GZ_EINVAL;
        for (uint32_t j = 0; j < i; j++) {
            if (!strcmp(ents[j].name, e->name))
                return GZ_EINVAL;
        }
    }
    return 0;
}

static const struct entry *find_entry(const struct entry *ents, uint32_t count, const char *name)
{
    for (uint32_t i = 0; i < count; i++) {
        if (!strcmp(ents[i].name, name))
            return &ents[i];
    }
    return NULL;
}

static int read_entry_info(int fd, const struct entry *ents, uint32_t count, struct pkg_info *info)
{
    const struct entry *e = find_entry(ents, count, "Info");
    char text[INFO_MAX];

    if (!e || e->size > sizeof(text))
        return GZ_EINVAL;
    if (fs_seek(fd, (int32_t)e->offset, FS_SEEK_SET) < 0 || fs_read(fd, text, e->size) != (int)e->size)
        return GZ_EIO;
    int rv = parse_info(text, e->size, info);
    if (!rv && !find_entry(ents, count, info->exec))
        rv = GZ_EINVAL;
    return rv;
}

int pkg_read_info(const char *gzp, struct pkg_info *info)
{
    static struct entry ents[MAX_ENTRIES];
    uint32_t count, size;
    int fd = fs_open(gzp, FS_O_READ);

    if (fd < 0)
        return fd;
    int rv = read_table(fd, ents, &count, &size);
    if (!rv)
        rv = read_entry_info(fd, ents, count, info);
    fs_close(fd);
    return rv;
}

int pkg_read_entry(const char *gzp, const char *name, void *buf, uint32_t max)
{
    static struct entry ents[MAX_ENTRIES];
    uint32_t count, size;
    int fd = fs_open(gzp, FS_O_READ);

    if (fd < 0)
        return fd;
    int rv = read_table(fd, ents, &count, &size);
    const struct entry *e = rv ? NULL : find_entry(ents, count, name);
    if (!rv && !e)
        rv = GZ_ENOENT;
    else if (!rv && e->size > max)
        rv = GZ_EFULL;
    else if (!rv && (fs_seek(fd, (int32_t)e->offset, FS_SEEK_SET) < 0 || fs_read(fd, buf, e->size) != (int)e->size))
        rv = GZ_EIO;
    fs_close(fd);
    return rv ? rv : (int)e->size;
}

int pkg_bundle_info(const char *bundle, struct pkg_info *info)
{
    char path[FS_PATH_MAX], text[INFO_MAX];

    snprintf(path, sizeof(path), "%s/Info", bundle);
    int n = fs_read_file(path, text, sizeof(text));
    if (n < 0)
        return n;
    return parse_info(text, (size_t)n, info);
}

static void bundle_path(char *out, size_t n, const char *root, const char *id, const char *suffix)
{
    snprintf(out, n, "%s/Applications/%s.app%s", root, id, suffix);
}

/* The ready marker moves with the staged bundle. A backup is retained until
 * the replacement and its data are visible. Renames are within one volume.
 * Recovery rolls back an interrupted pre-commit swap, or finishes cleanup
 * after commit. Cross-volume moves keep the source until destination commit. */
static int recover_one(const char *root, const char *id)
{
    char live[FS_PATH_MAX], bak[FS_PATH_MAX], tmp[FS_PATH_MAX], marker[FS_PATH_MAX];
    char source[PKG_BUNDLE_MAX] = {0}, flash[PKG_BUNDLE_MAX], card[PKG_BUNDLE_MAX];
    struct fs_stat st;
    struct pkg_info info;
    bundle_path(live, sizeof(live), root, id, "");
    bundle_path(bak, sizeof(bak), root, id, ".bak");
    bundle_path(tmp, sizeof(tmp), root, id, ".tmp");
    if (fs_stat(live, &st) == GZ_ENOENT && fs_stat(bak, &st) == 0) {
        int rv = fs_rename(bak, live);
        if (rv)
            return rv;
    }
    if (pkg_bundle_info(live, &info) == 0 && !strcmp(info.id, id)) {
        snprintf(marker, sizeof(marker), "%s/.pkg-ready", live);
        int n = fs_read_file(marker, source, sizeof(source) - 1);
        if (n >= 0) {
            source[n] = 0;
            bundle_path(flash, sizeof(flash), "", id, "");
            bundle_path(card, sizeof(card), "/Card", id, "");
            if (*source && strcmp(source, live) && (!strcmp(source, flash) || !strcmp(source, card))) {
                /* Defer source cleanup until its removable volume returns. */
                if (!strncmp(source, "/Card/", 6) && fs_stat("/Card", &st))
                    return GZ_ENOENT;
                int rv = fs_remove_all(source);
                if (rv && rv != GZ_ENOENT)
                    return rv;
            }
            int rv = fs_remove_all(bak);
            if (rv && rv != GZ_ENOENT)
                return rv;
            rv = fs_remove(marker);
            if (rv && rv != GZ_ENOENT)
                return rv;
        } else if (n != GZ_ENOENT)
            return n;
    }
    int rv = fs_remove_all(tmp);
    return rv == GZ_ENOENT ? 0 : rv;
}

int pkg_find(const char *id, char bundle[PKG_BUNDLE_MAX])
{
    static const char *const roots[] = {"", "/Card"};
    struct fs_stat st;

    if (!id_ok(id))
        return GZ_EINVAL;
    for (unsigned i = 0; i < 2; i++)
        recover_one(roots[i], id);
    for (unsigned i = 0; i < 2; i++) {
        bundle_path(bundle, PKG_BUNDLE_MAX, roots[i], id, "");
        if (fs_stat(bundle, &st) == 0 && st.type == FS_DIR)
            return 0;
    }
    return GZ_ENOENT;
}

/* Copies one entry of the open package to a file. */
static int extract(int fd, const struct entry *e, const char *path, uint8_t *buf, uint32_t bufsize)
{
    int out = fs_open(path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
    int rv = 0;

    if (out < 0)
        return out;
    for (uint32_t done = 0; done < e->size && !rv;) {
        uint32_t n = MIN(e->size - done, bufsize);
        if (fs_seek(fd, (int32_t)(e->offset + done), FS_SEEK_SET) < 0 || fs_read(fd, buf, n) != (int)n)
            rv = GZ_EIO;
        else if (fs_write(out, buf, n) != (int)n)
            rv = GZ_ENOSPC;
        done += n;
    }
    int crv = fs_close(out);
    return rv ? rv : crv;
}

/* Copy saved data into staging; never move it out of the working app.
 * Bounded recursion/path lengths also protect against malformed app data. */
static int copy_data(const char *src, const char *dst, uint8_t *buf, unsigned depth)
{
    struct fs_stat st;
    int rv = fs_stat(src, &st);
    if (rv)
        return rv;
    if (depth > 12)
        return GZ_EINVAL;
    if (st.type == FS_DIR) {
        rv = fs_mkdirs(dst);
        if (rv)
            return rv;
        struct fs_dirent ents[4];
        uint32_t skip = 0;
        int n;
        while ((n = fs_list(src, skip, ents, 4)) > 0) {
            skip += n;
            for (int i = 0; i < n; i++) {
                char a[FS_PATH_MAX], b[FS_PATH_MAX];
                if (snprintf(a, sizeof(a), "%s/%s", src, ents[i].name) >= (int)sizeof(a) ||
                    snprintf(b, sizeof(b), "%s/%s", dst, ents[i].name) >= (int)sizeof(b))
                    return GZ_EINVAL;
                rv = copy_data(a, b, buf, depth + 1);
                if (rv)
                    return rv;
            }
        }
        return n < 0 ? n : 0;
    }
    int in = fs_open(src, FS_O_READ);
    if (in < 0)
        return in;
    int out = fs_open(dst, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
    if (out < 0) {
        fs_close(in);
        return out;
    }
    uint32_t done = 0;
    while (done < st.size) {
        uint32_t n = MIN(st.size - done, FS_IO_MAX);
        int got = fs_read(in, buf, n);
        if (got != (int)n) {
            rv = GZ_EIO;
            break;
        }
        if (fs_write(out, buf, n) != (int)n) {
            rv = GZ_ENOSPC;
            break;
        }
        done += n;
    }
    fs_close(in);
    int crv = fs_close(out);
    return rv ? rv : crv;
}

int pkg_install(const char *gzp, const char *root, char bundle[PKG_BUNDLE_MAX])
{
    static struct entry ents[MAX_ENTRIES];
    struct pkg_info info;
    struct fs_info vol;
    struct fs_stat st;
    char tmp[FS_PATH_MAX], path[FS_PATH_MAX], old[PKG_BUNDLE_MAX] = {0};
    char bak[FS_PATH_MAX], source[FS_PATH_MAX];
    uint32_t count, size, need = 0;
    uint8_t *buf = NULL;
    int fd, rv;

    if (strcmp(root, "") && strcmp(root, "/Card"))
        return GZ_EINVAL;
    if (*root && (fs_stat(root, &st) || st.type != FS_DIR))
        return GZ_ENOENT; /* no card in */
    fd = fs_open(gzp, FS_O_READ);
    if (fd < 0)
        return fd;
    rv = read_table(fd, ents, &count, &size);
    if (!rv)
        rv = read_entry_info(fd, ents, count, &info);
    if (rv)
        goto out;
    rv = recover_one(root, info.id);
    if (rv)
        goto out;
    if (pkg_find(info.id, old) < 0)
        old[0] = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (ents[i].size > 0x7fffffffu - need - 1024) {
            rv = GZ_EFULL;
            goto out;
        }
        need += ents[i].size + 1024; /* data plus per-file overhead */
    }
    rv = fs_statvfs(*root ? root : "/", &vol);
    if (rv)
        goto out;
    if (vol.total - vol.used < need) {
        rv = GZ_ENOSPC;
        goto out;
    }
    buf = malloc(FS_IO_MAX);
    if (!buf) {
        rv = GZ_ENOMEM;
        goto out;
    }

    /* Unpack beside any old copy, then swap: an interrupted install never
     * looks like an app (pkg_list removes the leftover). */
    bundle_path(tmp, sizeof(tmp), root, info.id, ".tmp");
    fs_remove_all(tmp);
    for (uint32_t i = 0; i < count && !rv; i++) {
        snprintf(path, sizeof(path), "%s/%s", tmp, ents[i].name);
        char *slash = strrchr(path, '/');
        *slash = 0;
        rv = fs_mkdirs(path);
        *slash = '/';
        if (!rv)
            rv = extract(fd, &ents[i], path, buf, FS_IO_MAX);
    }
    if (!rv && *old) {
        snprintf(source, sizeof(source), "%s/data", old);
        snprintf(path, sizeof(path), "%s/data", tmp);
        int sr = fs_stat(source, &st);
        struct pkg_info previous;
        if (sr == GZ_ENOENT && pkg_bundle_info(old, &previous) == 0)
            rv = 0;
        else
            rv = copy_data(source, path, buf, 0);
    }
    if (!rv) {
        snprintf(path, sizeof(path), "%s/.pkg-ready", tmp);
        int n = (int)strlen(old) + 1;
        rv = fs_write_file(path, old, (uint32_t)n);
        rv = rv == n ? 0 : rv < 0 ? rv : GZ_EIO;
    }
    bundle_path(bundle, PKG_BUNDLE_MAX, root, info.id, "");
    bundle_path(bak, sizeof(bak), root, info.id, ".bak");
    bool backed = false;
    if (!rv && fs_stat(bundle, &st) == 0) {
        rv = fs_rename(bundle, bak);
        backed = !rv;
    }
    if (!rv)
        rv = fs_rename(tmp, bundle); /* commit point */
    if (rv) {
        if (backed)
            fs_rename(bak, bundle);
        fs_remove_all(tmp);
    } else {
        /* Cleanup can be retried by recovery; the new app is committed. */
        recover_one(root, info.id);
    }
out:
    free(buf);
    fs_close(fd);
    return rv;
}

int pkg_uninstall(const char *bundle)
{
    return fs_remove_all(bundle);
}

struct listed {
    char bundle[PKG_BUNDLE_MAX];
    struct pkg_info info;
};

static int listed_cmp(const void *a, const void *b)
{
    return strcasecmp(((const struct listed *)a)->info.name, ((const struct listed *)b)->info.name);
}

int pkg_list(const char *root, void (*fn)(const char *bundle, const struct pkg_info *info, void *arg), void *arg)
{
    static struct listed apps[MAX_LISTED];
    struct fs_dirent ents[8];
    char dir[FS_PATH_MAX], path[FS_PATH_MAX];
    uint32_t skip = 0;
    int n, napps = 0;

    snprintf(dir, sizeof(dir), "%s/Applications", root);
    /* First recover every interrupted transaction. Do not change directory
     * enumeration offsets while scanning: restart after each recovered id. */
    bool restart = true;
    for (int pass = 0; restart && pass < 128; pass++) {
        restart = false;
        skip = 0;
        while (!restart && (n = fs_list(dir, skip, ents, 8)) > 0) {
            skip += (uint32_t)n;
            for (int i = 0; i < n; i++) {
                size_t len = strlen(ents[i].name);
                bool txn = len > 8 && (!strcmp(ents[i].name + len - 8, ".app.tmp") ||
                                       !strcmp(ents[i].name + len - 8, ".app.bak"));
                bool app = len > 4 && !strcmp(ents[i].name + len - 4, ".app");
                if (ents[i].type != FS_DIR || (!txn && !app))
                    continue;
                char id[PKG_ID_MAX + 1], marker[FS_PATH_MAX];
                size_t idlen = len - (txn ? 8 : 4);
                if (idlen > PKG_ID_MAX)
                    continue;
                memcpy(id, ents[i].name, idlen);
                id[idlen] = 0;
                if (!id_ok(id))
                    continue;
                snprintf(marker, sizeof(marker), "%s/%s/.pkg-ready", dir, ents[i].name);
                struct fs_stat st;
                if (txn || fs_stat(marker, &st) == 0) {
                    if (recover_one(root, id) == 0)
                        restart = true;
                    if (restart)
                        break;
                }
            }
        }
    }
    skip = 0;
    while ((n = fs_list(dir, skip, ents, 8)) > 0) {
        skip += (uint32_t)n;
        for (int i = 0; i < n; i++) {
            size_t len = strlen(ents[i].name);
            if (ents[i].type != FS_DIR)
                continue;
            snprintf(path, sizeof(path), "%s/%s", dir, ents[i].name);
            if (len <= 4 || strcmp(ents[i].name + len - 4, ".app") || napps == MAX_LISTED ||
                strlen(path) >= PKG_BUNDLE_MAX)
                continue;
            struct listed *a = &apps[napps];
            /* The folder must match its Info, so the id stays unique. */
            if (pkg_bundle_info(path, &a->info) == 0 && strlen(a->info.id) == len - 4 &&
                !memcmp(a->info.id, ents[i].name, len - 4)) {
                strlcpy(a->bundle, path, sizeof(a->bundle));
                napps++;
            }
        }
    }
    qsort(apps, (size_t)napps, sizeof(apps[0]), listed_cmp);
    for (int i = 0; i < napps; i++)
        fn(apps[i].bundle, &apps[i].info, arg);
    return napps;
}
