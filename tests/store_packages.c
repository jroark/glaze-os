/* Exercise the production installer on POSIX-backed volumes, cutting the
 * process before every mutation. Child exit simulates a power interruption. */
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glaze/fs.h>
#include <glaze/pkg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static char root[512];
static int cut, mutations, full;
static bool card_missing, lose_source, lose_destination;
static void mutation(void)
{
    if (cut && ++mutations == cut)
        _exit(77);
}
static int err(void) { return errno == ENOENT ? GZ_ENOENT : errno == ENOSPC ? GZ_ENOSPC : GZ_EIO; }
static void hostpath(char *out, const char *path)
{
    snprintf(out, 768, "%s%s%s", root, card_missing && !strncmp(path, "/Card", 5) ? "/AbsentCard" : "", path);
}
int fs_stat(const char *path, struct fs_stat *out)
{
    char p[768];
    hostpath(p, path);
    struct stat st;
    if (stat(p, &st))
        return err();
    out->type = S_ISDIR(st.st_mode) ? FS_DIR : FS_FILE;
    out->size = (uint32_t)st.st_size;
    return 0;
}
int fs_statvfs(const char *p, struct fs_info *out)
{
    (void)p;
    memset(out, 0, sizeof(*out));
    out->total = 16 * 1024 * 1024;
    return 0;
}
int fs_open(const char *path, int flags)
{
    char p[768];
    hostpath(p, path);
    int mode = (flags & FS_O_WRITE) ? O_WRONLY : O_RDONLY;
    if (flags & FS_O_CREATE)
        mode |= O_CREAT;
    if (flags & FS_O_TRUNC)
        mode |= O_TRUNC;
    if (flags & FS_O_WRITE)
        mutation();
    int fd = open(p, mode, 0600);
    return fd < 0 ? err() : fd;
}
int fs_close(int fd) { return close(fd) ? err() : 0; }
int fs_read(int fd, void *buf, uint32_t n)
{
    int rv = (int)read(fd, buf, n);
    return rv < 0 ? err() : rv;
}
int fs_write(int fd, const void *buf, uint32_t n)
{
    mutation();
    if (full)
        return GZ_ENOSPC;
    int rv = (int)write(fd, buf, n);
    return rv < 0 ? err() : rv;
}
int fs_seek(int fd, int32_t off, int whence)
{
    off_t rv = lseek(fd, off, whence == FS_SEEK_SET ? SEEK_SET : whence == FS_SEEK_END ? SEEK_END : SEEK_CUR);
    return rv < 0 ? err() : (int)rv;
}
int fs_read_file(const char *p, void *buf, uint32_t n)
{
    int fd = fs_open(p, FS_O_READ);
    if (fd < 0)
        return fd;
    int rv = fs_read(fd, buf, n);
    fs_close(fd);
    return rv;
}
int fs_write_file(const char *p, const void *buf, uint32_t n)
{
    int fd = fs_open(p, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
    if (fd < 0)
        return fd;
    int rv = fs_write(fd, buf, n);
    fs_close(fd);
    return rv;
}
int fs_mkdir(const char *path)
{
    char p[768];
    hostpath(p, path);
    mutation();
    return mkdir(p, 0700) ? err() : 0;
}
int fs_mkdirs(const char *path)
{
    char p[FS_PATH_MAX];
    if (strlen(path) >= sizeof(p))
        return GZ_EINVAL;
    strcpy(p, path);
    struct fs_stat st;
    for (char *c = p + 1;; c++)
        if (*c == '/' || !*c) {
            char saved = *c;
            *c = 0;
            if (fs_stat(p, &st) < 0) {
                int rv = fs_mkdir(p);
                if (rv)
                    return rv;
            }
            *c = saved;
            if (!saved)
                break;
        }
    return 0;
}
int fs_list(const char *path, uint32_t skip, struct fs_dirent *out, int max)
{
    char p[768];
    hostpath(p, path);
    struct dirent **entries;
    int count = scandir(p, &entries, NULL, alphasort);
    if (count < 0)
        return err();
    int got = 0;
    unsigned index = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i]->d_name, ".") && strcmp(entries[i]->d_name, "..")) {
            if (index++ >= skip && got < (int)max) {
                struct fs_dirent *e = &out[got++];
                memset(e, 0, sizeof(*e));
                strlcpy(e->name, entries[i]->d_name, sizeof(e->name));
                e->type = entries[i]->d_type == DT_DIR ? FS_DIR : FS_FILE;
            }
        }
        free(entries[i]);
    }
    free(entries);
    return got;
}
int fs_remove(const char *path)
{
    char p[768];
    hostpath(p, path);
    struct stat st;
    if (stat(p, &st))
        return err();
    mutation();
    return (S_ISDIR(st.st_mode) ? rmdir(p) : unlink(p)) ? err() : 0;
}
int fs_remove_all(const char *path)
{
    struct fs_stat st;
    int rv = fs_stat(path, &st);
    if (rv)
        return rv;
    if (st.type == FS_DIR) {
        struct fs_dirent e;
        while ((rv = fs_list(path, 0, &e, 1)) == 1) {
            char p[FS_PATH_MAX];
            assert(snprintf(p, sizeof(p), "%s/%s", path, e.name) < (int)sizeof(p));
            rv = fs_remove_all(p);
            if (rv)
                return rv;
        }
        if (rv < 0)
            return rv;
    }
    return fs_remove(path);
}
int fs_rename(const char *src, const char *dst)
{
    bool commit = strstr(src, ".app.tmp") != NULL;
    if (commit && lose_destination && !strncmp(dst, "/Card/", 6))
        card_missing = true;
    char a[768], b[768];
    hostpath(a, src);
    hostpath(b, dst);
    mutation();
    int rv = rename(a, b) ? err() : 0;
    if (!rv && commit && lose_source && strncmp(dst, "/Card/", 6))
        card_missing = true;
    return rv;
}
struct entry {
    char name[48];
    uint32_t offset, size;
};
static void package(const char *path, const char *version, bool forbidden)
{
    char info[128];
    int n = snprintf(info, sizeof(info), "id=demo\nname=Demo\nversion=%s\nexec=app\n", version);
    struct entry e[3] = {{"Info", 0, 0}, {"app", 0, 4}, {"data/evil", 0, 4}};
    uint32_t count = forbidden ? 3 : 2;
    e[0].offset = 8 + count * sizeof(*e);
    e[0].size = n;
    e[1].offset = e[0].offset + n;
    e[2].offset = e[1].offset + 4;
    int fd = fs_open(path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
    assert(fd >= 0);
    assert(fs_write(fd, "GZPK", 4) == 4 && fs_write(fd, &count, 4) == 4 &&
           fs_write(fd, e, count * sizeof(*e)) == (int)(count * sizeof(*e)));
    assert(fs_write(fd, info, n) == n && fs_write(fd, version[0] == '1' ? "APP1" : "APP2", 4) == 4);
    if (forbidden)
        assert(fs_write(fd, "evil", 4) == 4);
    fs_close(fd);
}
static void setup(bool source_card)
{
    strcpy(root, "/private/tmp/glaze-pkg-test-XXXXXX");
    assert(mkdtemp(root));
    assert(!fs_mkdirs("/Applications") && !fs_mkdirs("/Card/Applications"));
    package("/old.gzp", "1.0", false);
    package("/new.gzp", "2.0", false);
    char bundle[PKG_BUNDLE_MAX], data[FS_PATH_MAX];
    assert(!pkg_install("/old.gzp", source_card ? "/Card" : "", bundle));
    snprintf(data, sizeof(data), "%s/data/nested", bundle);
    assert(!fs_mkdirs(data));
    snprintf(data, sizeof(data), "%s/data/nested/save", bundle);
    assert(fs_write_file(data, "remember me", 11) == 11);
}
static void verify(void)
{
    char bundle[PKG_BUNDLE_MAX], path[FS_PATH_MAX], buf[32] = {0};
    struct pkg_info info;
    assert(!pkg_find("demo", bundle) && !pkg_bundle_info(bundle, &info));
    assert(!strcmp(info.version, "1.0") || !strcmp(info.version, "2.0"));
    snprintf(path, sizeof(path), "%s/data/nested/save", bundle);
    assert(fs_read_file(path, buf, 32) == 11 && !memcmp(buf, "remember me", 11));
    snprintf(path, sizeof(path), "%s/app", bundle);
    assert(fs_read_file(path, buf, 32) == 4 && !memcmp(buf, info.version[0] == '1' ? "APP1" : "APP2", 4));
}
static void teardown(void) { assert(!fs_remove_all("")); }
int main(void)
{
    for (int mode = 0; mode < 3; mode++) {
        bool source_card = mode == 2;
        const char *dest = mode == 1 ? "/Card" : "";
        int tried = 0;
        for (int step = 1; step < 250; step++) {
            setup(source_card);
            pid_t pid = fork();
            assert(pid >= 0);
            if (!pid) {
                cut = step;
                char bundle[PKG_BUNDLE_MAX];
                int rv = pkg_install("/new.gzp", dest, bundle);
                _exit(rv ? 2 : 0);
            }
            int status;
            waitpid(pid, &status, 0);
            assert(WIFEXITED(status));
            int code = WEXITSTATUS(status);
            assert(code == 77 || code == 0);
            verify();
            teardown();
            tried = step;
            if (code == 0)
                break;
        }
        assert(tried < 249);
        printf("package recovery mode %d: %d interruption points PASS\n", mode, tried - 1);
    }
    setup(false);
    full = 1;
    char bundle[PKG_BUNDLE_MAX];
    assert(pkg_install("/new.gzp", "", bundle) < 0);
    full = 0;
    verify();
    package("/evil.gzp", "2.0", true);
    assert(pkg_install("/evil.gzp", "", bundle) == GZ_EINVAL);
    verify();
    teardown();
    /* Removal of the source card after a cross-volume commit defers cleanup. */
    setup(true);
    lose_source = true;
    assert(pkg_install("/new.gzp", "", bundle) == 0 && card_missing);
    verify();
    struct fs_stat st;
    assert(fs_stat("/Applications/demo.app/.pkg-ready", &st) == 0);
    assert(pkg_install("/new.gzp", "", bundle) < 0); /* Retain pending source cleanup. */
    card_missing = false;
    lose_source = false;
    assert(pkg_find("demo", bundle) == 0);
    assert(fs_stat("/Card/Applications/demo.app", &st) == GZ_ENOENT);
    assert(fs_stat("/Applications/demo.app/.pkg-ready", &st) == GZ_ENOENT);
    verify();
    teardown();
    /* Removal between the two rename operations leaves a recoverable backup. */
    setup(true);
    lose_destination = true;
    assert(pkg_install("/new.gzp", "/Card", bundle) < 0 && card_missing);
    card_missing = false;
    lose_destination = false;
    verify();
    struct pkg_info info;
    assert(pkg_find("demo", bundle) == 0 && pkg_bundle_info(bundle, &info) == 0);
    assert(!strcmp(info.version, "1.0"));
    teardown();
    puts("store packages: PASS (including card removal and deferred cleanup)");
}
