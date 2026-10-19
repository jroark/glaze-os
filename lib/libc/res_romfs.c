/* Device resource lookup: assets live in romfs under "res/". An installed
 * app also finds its own in its bundle's res/ folder, read from the file
 * system and kept for the life of the process (parsed images and fonts point
 * into the data, as they do into romfs mappings). */

#include <glaze/fs.h>
#include <glaze/res.h>
#include <glaze/sys.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char bundle[48];

void res_set_bundle(const char *path)
{
    strlcpy(bundle, path ? path : "", sizeof(bundle));
}

const char *res_bundle(void)
{
    return bundle;
}

const void *res_map(const char *name, uint32_t *size)
{
    char path[FS_PATH_MAX];
    struct fs_stat st;

    snprintf(path, sizeof(path), "res/%s", name);
    const void *data = gz_romfs_map(path, size);
    if (data || !bundle[0])
        return data;
    snprintf(path, sizeof(path), "%s/res/%s", bundle, name);
    if (fs_stat(path, &st) || st.type != FS_FILE || !st.size)
        return NULL;
    void *buf = malloc(st.size);
    if (buf && fs_read_file(path, buf, st.size) != (int)st.size) {
        free(buf);
        buf = NULL;
    }
    if (buf)
        *size = st.size;
    return buf;
}
