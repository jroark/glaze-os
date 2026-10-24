#include "../third_party/totalsms/sms.h"
#include "../third_party/totalsms/sms_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    uint8_t *rom=calloc(1,65536);struct SMS_Core *s=calloc(1,sizeof *s);assert(rom && s);
    for(unsigned bank=0;bank<4;bank++) memset(rom+bank*16384,bank,16384);
    memcpy(rom+0x7ff0,"TMR SEGA",8);rom[0x7fff]=0x4e;
    assert(SMS_init(s));assert(SMS_loadromEx(s,rom,65536,SMS_System_SMS,SMS_Region_NTSC,SMS_Console_EXPORT));
    SMS_write8(s,0xfffe,3);assert(SMS_read8(s,0x4000)==3);
    SMS_write8(s,0xfffd,2);assert(SMS_read8(s,0)==0 && SMS_read8(s,0x400)==2);
    SMS_write8(s,0xffff,1);assert(SMS_read16(s,0x7fff)==0x0103); /* crossing independently selected banks */
    SMS_write8(s,0xfffc,8);SMS_write8(s,0x8000,0x91);
    SMS_write8(s,0xfffc,12);SMS_write8(s,0x8000,0x62);
    SMS_write8(s,0xfffc,8);assert(SMS_read8(s,0x8000)==0x91);
    SMS_write16(s,0xdfff,0xa152);assert(SMS_read8(s,0xdfff)==0x52 && SMS_read8(s,0xe000)==0xa1);
    SMS_write8(s,0xffff,0);assert((SMS_read16(s,0xffff)>>8)==SMS_read8(s,0));
    /* Codemasters register and RAM mapping. */
    s->cart.mapper_type=MAPPER_TYPE_CODEMASTERS;mapper_init(s);
    SMS_write8(s,0,2);SMS_write8(s,0x4000,1);SMS_write8(s,0x8000,3);
    assert(SMS_read8(s,0)==2 && SMS_read8(s,0x4000)==1 && SMS_read8(s,0x8000)==3);
    /* Small cartridges mirror safely through all three ROM windows. */
    memset(rom,0x17,8192);memcpy(rom+0x1ff0,"TMR SEGA",8);rom[0x1fff]=0x4a;
    assert(SMS_loadromEx(s,rom,8192,SMS_System_SMS,SMS_Region_NTSC,SMS_Console_EXPORT));
    assert(SMS_read8(s,0x6000)==0x17);assert(SMS_read8(s,0x8000)==0x17);
    rom[0]=0xc3;rom[1]=0;rom[2]=1;rom[0x100]=0xc3;rom[0x101]=0;rom[0x102]=1;
    uint16_t *pixels=malloc((256*240+16)*sizeof *pixels);assert(pixels);
    for(unsigned i=0;i<256*240+16;i++) pixels[i]=0x1234;
    SMS_set_pixels(s,pixels,256,16);s->vdp.registers[0]=6;s->vdp.registers[1]=0x50;
    for(int i=0;i<2;i++) SMS_run(s,SMS_RunEndFrame);
    assert(pixels[256*223]!=0x1234 && pixels[256*239]==0x1234);
    s->vdp.registers[1]=0x48;
    for(int i=0;i<2;i++) SMS_run(s,SMS_RunEndFrame);
    assert(pixels[256*239]!=0x1234);
    for(unsigned i=256*240;i<256*240+16;i++) assert(pixels[i]==0x1234);
    free(pixels);
    SMS_quit(s);free(s);free(rom);puts("SMS mapper PASS: Sega/Codemasters, fixed bank, SRAM banks, mirrored bus, word boundaries, 8 KiB ROM and 224/240-line framebuffer bounds");return 0;
}
