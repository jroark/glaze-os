#include "../../servers/springboard/installer.h"
#include "../../host/hostres.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint16_t pixels[240*320];
int main(void)
{
    struct sb_installer s;
    sb_installer_init(&s,false,GZ_INSTALL_MAGIC,GZ_INSTALL_MAGIC_SIZE);assert(!s.active);
    sb_installer_init(&s,true,NULL,0);assert(!s.active);
    sb_installer_init(&s,true,"invalid!",8);assert(!s.active);
    sb_installer_init(&s,true,GZ_INSTALL_MAGIC,GZ_INSTALL_MAGIC_SIZE);assert(s.active && s.stage==GZ_INSTALL_WAIT);
    assert(sb_installer_update(&s,GZ_INSTALL_STAGE_COUNT,0,0)<0);
    assert(sb_installer_update(&s,GZ_INSTALL_READ,11,10)<0);
    gfx_surface surface;gfx_surface_init(&surface,pixels,240,320,240);
    sb_installer_draw(&surface,&s,0);host_write_bmp("../out/host/installer-wait.bmp",&surface);
    assert(!sb_installer_update(&s,GZ_INSTALL_WRITE,54,100));
    sb_installer_draw(&surface,&s,0);host_write_bmp("../out/host/installer-write.bmp",&surface);
    assert(!sb_installer_update(&s,GZ_INSTALL_ERROR,0,0));
    sb_installer_draw(&surface,&s,0);host_write_bmp("../out/host/installer-error.bmp",&surface);
    sb_installer_init(&s,true,GZ_RESTORE_MAGIC,GZ_INSTALL_MAGIC_SIZE);assert(s.active && s.restoring);
    sb_installer_draw(&surface,&s,0);host_write_bmp("../out/host/installer-restore.bmp",&surface);
    sb_installer_init(&s,false,NULL,0);assert(!s.active && sb_installer_update(&s,GZ_INSTALL_READ,0,1)<0);
    puts("installer-ui: RAM marker gating, status bounds, progress, error and NAND reset PASS");return 0;
}
