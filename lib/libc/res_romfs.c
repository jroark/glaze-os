/* Device resource lookup: assets live in romfs under "res/". */

#include <glaze/res.h>
#include <glaze/sys.h>
#include <stdio.h>

const void *res_map(const char *name, uint32_t *size)
{
    char path[48];

    snprintf(path, sizeof(path), "res/%s", name);
    return gz_romfs_map(path, size);
}
