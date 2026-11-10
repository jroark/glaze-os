/* A PE executable's first icon group, scaled to a square GZI with alpha. */
#ifndef WINCE_EXEICON_H
#define WINCE_EXEICON_H
#include "assets.h"
/* Reads the executable's bytes through `read`; *gzi is malloc'd. */
const char *ce_exe_icon(ce_asset_reader read, void *arg, uint32_t bytes, unsigned size, uint8_t **gzi, uint32_t *gzi_bytes);
/* Where Files and the runner keep a NAND executable's icon: FNV-1a of its
 * source path, as SpringBoard names direct-launch contexts. An empty file
 * records an executable without one. */
#define CE_ICON_DIR "/Library/WinCE/icons"
#define CE_ICON_SIZE 24
/* <hash>-32.gzi: the launched program's own icon for SpringBoard's home
 * screen (servers/springboard/apps.c puts it on a tile). */
#define CE_ICON_HOME_SIZE 32
#endif
