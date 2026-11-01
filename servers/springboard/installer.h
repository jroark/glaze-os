#ifndef SB_INSTALLER_H
#define SB_INSTALLER_H
#include <glaze/gfx.h>
#include <glaze/install.h>
struct sb_installer { bool active, restoring; uint32_t stage, current, total; };
void sb_installer_init(struct sb_installer *, bool ram, const void *marker, uint32_t size);
int sb_installer_update(struct sb_installer *, uint32_t stage, uint32_t current, uint32_t total);
void sb_installer_draw(gfx_surface *, const struct sb_installer *, uint32_t time);
#endif
