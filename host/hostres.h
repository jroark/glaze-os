/* Host-side stand-ins for romfs resource mapping and BMP output. */
#ifndef GLAZE_HOSTRES_H
#define GLAZE_HOSTRES_H

#include <glaze/gfx.h>

const void *host_load(const char *path, size_t *size);
void host_write_bmp(const char *path, const gfx_surface *s);

#endif
