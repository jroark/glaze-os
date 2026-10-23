#include "../third_party/totalsms/sms.h"
#include "../third_party/totalsms/sms_internal.h"
#include "../third_party/totalsms/sn76489.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool saw_twelve_bits;
static uint32_t colour(void *u,uint8_t r,uint8_t g,uint8_t b) {
    (void)u;assert(r<16 && g<16 && b<16);
    if(r==13 && g==10 && b==3)saw_twelve_bits=true;
    return ((r*31/15)<<11)|((g*63/15)<<5)|(b*31/15);
}
static void routing(unsigned mask,bool left,bool right) {
    Sn76489 *p=psg_init(3579545,22050);assert(p);
    psg_write_io(p,0x80,0);psg_write_io(p,8,0);psg_write_io(p,0x90,0);
    psg_gg_write_io(p,mask,0);psg_end_frame(p,59500);
    int16_t out[2048];int n=psg_read_samples(p,out,1024);assert(n>300);
    unsigned l=0,r=0;for(int i=0;i<n;i++) { l+=out[i*2]!=0;r+=out[i*2+1]!=0; }
    assert((l>0)==left && (r>0)==right);psg_quit(p);
}
int main(int argc,char **argv) {
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);
    uint8_t *rom=malloc(32768);assert(rom && fread(rom,1,32768,f)==32768);fclose(f);
    struct SMS_Core *s=calloc(1,sizeof *s);assert(s && SMS_init(s));
    uint16_t *pixels=malloc((256*240+16)*2);assert(pixels);
    for(unsigned i=0;i<256*240+16;i++)pixels[i]=0x1234;
    SMS_set_colour_callback(s,colour);SMS_set_pixels(s,pixels,256,16);
    assert(SMS_loadromEx(s,rom,32768,SMS_System_GG,SMS_Region_NTSC,SMS_Console_EXPORT));
    assert(SMS_read_io(s,0)&128);SMS_set_buttons(s,1u<<13,true);
    assert(!(SMS_read_io(s,0)&128) && !s->vdp.nmi_pending);
    SMS_set_buttons(s,1u<<13,false);assert(SMS_read_io(s,0)&128);
    for(unsigned i=0;i<30;i++)SMS_run(s,SMS_RunEndFrame);
    assert(saw_twelve_bits);
    for(unsigned i=256*240;i<256*240+16;i++)assert(pixels[i]==0x1234);
    SMS_quit(s);free(s);free(rom);free(pixels);
    routing(0x10,true,false);routing(1,false,true);routing(0,false,false);routing(0x11,true,true);
    puts("Game Gear hardware PASS: polled Start/no NMI, 12-bit palette, framebuffer bounds, left/right/mute PSG routing");
}
