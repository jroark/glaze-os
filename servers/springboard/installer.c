#include "installer.h"
#include <glaze/res.h>
#include <string.h>
void sb_installer_init(struct sb_installer *s, bool ram, const void *marker, uint32_t size)
{
    memset(s,0,sizeof(*s));
    s->restoring=marker && size==GZ_INSTALL_MAGIC_SIZE && !memcmp(marker,GZ_RESTORE_MAGIC,size);
    s->active=ram && marker && size==GZ_INSTALL_MAGIC_SIZE && (s->restoring || !memcmp(marker,GZ_INSTALL_MAGIC,size));
}
int sb_installer_update(struct sb_installer *s,uint32_t stage,uint32_t current,uint32_t total)
{
    if(!s->active || stage>=GZ_INSTALL_STAGE_COUNT || current>total) return -1;
    s->stage=stage;s->current=current;s->total=total;return 0;
}
static void centered(gfx_surface *s,int y,int font,const char *text,uint32_t color)
{
    const gfx_font *f=ui_font(font);
    gfx_text(s,(s->w-gfx_text_width(f,text))/2,y,f,text,color,255);
}
void sb_installer_draw(gfx_surface *s,const struct sb_installer *v,uint32_t time)
{
    static const char *const stages[]={"Waiting for your computer", "Reading your device...",
        "Preparing your apps...", "Checking your device...", "Installing Glaze...",
        "Verifying installation...", "Restarting your BE-300...", "Check your computer"};
    gfx_fill(s,gfx_r(0,0,s->w,s->h),0x101a2b);
    centered(s,32,FONT_B10,v->stage==GZ_INSTALL_ERROR?"INSTALLATION PAUSED":v->restoring?"RESTORING WINDOWS CE":"INSTALLING GLAZE",0x72d8d0);
    centered(s,59,FONT_R44,"Glaze",0xffffff);
    centered(s,126,FONT_R12,"Glaze is running temporarily.",0xc7d4e7);
    centered(s,145,FONT_B12,v->restoring?"Recovery is not finished.":"Installation is not finished.",0xffffff);
    centered(s,190,FONT_B12,v->restoring && v->stage==GZ_INSTALL_WRITE?"Restoring Windows CE...":stages[v->stage<GZ_INSTALL_STAGE_COUNT?v->stage:0],0xffffff);
    if(v->total) {
        gfx_round_rect(s,gfx_r(24,223,192,8),4,0x2c3e58,255);
        unsigned width=(uint64_t)v->current*192/v->total;
        if(width)gfx_round_rect(s,gfx_r(24,223,(int)width,8),4,0x72d8d0,255);
    } else {
        for(unsigned i=0;i<3;i++)gfx_round_rect(s,gfx_r(102+(int)i*14,223,7,7),3,
                                             (time/450)%3==i?0x72d8d0:0x2c3e58,255);
    }
    centered(s,270,FONT_R11,"Continue on your computer.",0xc7d4e7);
    centered(s,290,FONT_R10,"Keep USB and power connected.",0x9dafc8);
}
