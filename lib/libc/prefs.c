/* Preferences as files (see glaze/prefs.h). */

#include <glaze/prefs.h>
#include <glaze/fs.h>
#include <stdio.h>
#include <string.h>

static int path_for(const char *key, char *path, size_t n)
{
    int len = snprintf(path, n, PREFS_DIR "/%s", key);

    return len > 0 && (size_t)len < n && !strchr(key, '/') ? 0 : GZ_EINVAL;
}

int pref_get(const char *key, void *buf, uint32_t len)
{
    char path[FS_PATH_MAX];
    int rv = path_for(key, path, sizeof(path));

    return rv ? rv : fs_read_file(path, buf, len);
}

int pref_put(const char *key, const void *buf, uint32_t len)
{
    char path[FS_PATH_MAX];
    int rv = path_for(key, path, sizeof(path));

    if (rv)
        return rv;
    rv = fs_write_file(path, buf, len);
    if (rv == GZ_ENOENT && fs_mkdirs(PREFS_DIR) == 0)
        rv = fs_write_file(path, buf, len);
    return rv < 0 ? rv : 0;
}

int pref_delete(const char *key)
{
    char path[FS_PATH_MAX];
    int rv = path_for(key, path, sizeof(path));

    return rv ? rv : fs_remove(path);
}
