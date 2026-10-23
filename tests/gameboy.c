#include "../packages/gameboy/gameboy.h"
#include "../third_party/gnuboy/cpu.h"
#include "../third_party/gnuboy/hw.h"
#include "../third_party/gnuboy/mem.h"
#include "../third_party/gnuboy/fastmem.h"
#include "../third_party/gnuboy/regs.h"
#include "../third_party/gnuboy/lcd.h"
#include "../third_party/gnuboy/rtc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned samples,nonzero;
void gb_audio(const int16_t *p,unsigned n) { samples+=n; while(n--) nonzero+=*p++!=0; }
void gb_serial(uint8_t v) { (void)v; }
void gb_log(const char *s) { fprintf(stderr,"core: %s\n",s); }
static uint8_t *readrom(const char *path,size_t *n) {
    FILE *f=fopen(path,"rb"); assert(f); fseek(f,0,SEEK_END); *n=ftell(f); rewind(f);
    uint8_t *p=malloc(*n); assert(p); assert(fread(p,1,*n,f)==*n); fclose(f); return p;
}
static uint8_t *synthetic(unsigned code,unsigned type,unsigned ram_code) {
    uint32_t size=32768u<<code;
    uint8_t *p=calloc(1,size); assert(p);
    for(unsigned bank=0;bank<size/16384;bank++) {
        p[bank*16384]=bank; p[bank*16384+1]=bank>>8;
    }
    p[0x147]=type; p[0x148]=code; p[0x149]=ram_code;
    return p;
}
static void cartridge_tests(void) {
    struct gb_cart c;
    uint8_t *p=synthetic(0,0,0);
    for(unsigned n=0;n<0x150;n++) assert(gb_validate(p,n,&c));
    assert(!gb_validate(p,32768,&c));
    assert(gb_validate(p,32767,&c)); assert(gb_validate(p,32769,&c));
    p[0x148]=9; assert(gb_validate(p,32768,&c)); p[0x148]=0;
    p[0x147]=0x22; assert(gb_validate(p,32768,&c));
    p[0x147]=0; p[0x149]=2; assert(gb_validate(p,32768,&c));
    p[0x147]=3; p[0x149]=4; assert(gb_validate(p,32768,&c));
    p[0x147]=0; p[0x149]=0; p[0x143]=0xc0;
    assert(!gb_validate(p,32768,&c) && c.cgb && c.checksum_warning);
    assert(!gb_open(p,32768,1000));
    assert(readb(0xa000)==255); writeb(0xa000,1);
    writeb(0xffff,0x12); assert((readw(0xffff)&255)==0x12);
    gb_close(); free(p);
    p=synthetic(6,3,3); assert(!gb_open(p,32768u<<6,1000));
    writeb(0x2000,0); assert(readb(0x4000)==1);
    writeb(0x4000,2); assert(readb(0x4000)==65);
    writeb(0x6000,1); assert(readb(0)==64); assert(mbc.rambank==2);
    writeb(0,10); writeb(0xa000,0x52);
    writeb(0x4000,1); assert(readb(0xa000)==0);
    writeb(0x4000,2); assert(readb(0xa000)==0x52);
    writeb(0x6000,0); assert(readb(0)==0); assert(mbc.rambank==0);
    gb_reset(); assert(readb(0x4000)==1 && mbc.model==0);
    gb_close(); free(p);
    p=synthetic(3,6,0); assert(!gb_open(p,32768u<<3,1000));
    assert(gb_cartridge.ram_size==512); assert(readb(0xa000)==255);
    writeb(0,10); writeb(0xa000,0xab); assert(readb(0xa200)==0xfb);
    writeb(0xbfff,0xc7); assert(readb(0xa1ff)==0xf7);
    writeb(0x2100,0); assert(readb(0x4000)==1);
    writeb(0x2100,3); assert(readb(0x4000)==3);
    writeb(0,0); assert(readb(0xa000)==255);
    gb_close(); free(p);
    p=synthetic(0,9,1); assert(!gb_open(p,32768,1000));
    writeb(0xa000,0x77); assert(readb(0xa800)==0x77);
    gb_close(); free(p);
    p=synthetic(8,27,4); assert(!gb_open(p,GB_ROM_MAX,1000));
    writeb(0x2000,255); writeb(0x3000,1);
    assert(readb(0x4000)==255 && readb(0x4001)==1);
    writeb(0x2000,0); writeb(0x3000,0); assert(readb(0x4000)==0);
    writeb(0,10); writeb(0x4000,15); writeb(0xbfff,0x81);
    assert(gb_ram()[131071]==0x81);
    gb_close(); free(p);
    p=synthetic(0,30,5); assert(!gb_open(p,32768,1000));
    writeb(0,10); writeb(0x4000,15); writeb(0xbfff,0x92);
    assert(gb_ram()[65535]==0x92);
    gb_close(); free(p);
    puts("Game Boy cartridge PASS: malformed headers, ROM-only, MBC1/2/5, 8 MiB/128 KiB boundaries");
}
static void clock_tests(void) {
    uint8_t *p=synthetic(0,16,2); assert(!gb_open(p,32768,1000));
    writeb(0,10); writeb(0x4000,8); writeb(0xa000,59);
    writeb(0x4000,9); writeb(0xa000,59);
    writeb(0x4000,10); writeb(0xa000,23);
    writeb(0x4000,11); writeb(0xa000,255);
    writeb(0x4000,12); writeb(0xa000,1);
    gb_clock_sync(1001); assert(rtc.d==0 && rtc.s==0 && rtc.carry);
    writeb(0x6000,0); writeb(0x6000,1);
    writeb(0x4000,12); assert(readb(0xa000)==128);
    gb_clock_sync(1011); writeb(0x4000,8); assert(readb(0xa000)==0); /* latched */
    writeb(0x6000,0); writeb(0x6000,1); assert(readb(0xa000)==10);
    writeb(0x4000,12); writeb(0xa000,64); gb_clock_sync(1100); assert(rtc.s==10 && rtc.stop && !rtc.carry);
    writeb(0xa000,0); gb_clock_sync(1102); assert(rtc.s==12);
    uint8_t save[GB_RTC_SIZE]; gb_clock_export(save,1102);
    rtc.s=0; assert(gb_clock_import(save,1162)); assert(rtc.m==1 && rtc.s==12);
    assert(gb_clock_import(save,1000)); assert(rtc.m==0 && rtc.s==12);
    save[0]^=1; assert(!gb_clock_import(save,1200)); save[0]^=1;
    save[5]^=1; assert(!gb_clock_import(save,1200)); save[5]^=1;
    assert(gb_clock_import(save,1162)); gb_reset(); assert(rtc.m==1 && rtc.s==12);
    gb_clock_sync(1162+86400u*513u); assert(rtc.d==1 && rtc.carry);
    writeb(0,10); writeb(0x4000,7); assert(readb(0xa000)==255);
    writeb(0x4000,15); writeb(0xa000,88); assert(readb(0xa000)==255);
    gb_close(); free(p);
    p=synthetic(0,15,0); assert(!gb_open(p,32768,1000));
    assert(gb_cartridge.battery && gb_cartridge.clock && !gb_cartridge.ram_size);
    writeb(0,10); writeb(0x4000,8); writeb(0xa000,41); gb_clock_sync(1001); assert(rtc.s==42);
    gb_close(); free(p);
    puts("Game Boy RTC PASS: selection, latch, halt, day overflow, offline/backward time, integrity, reset");
}
static void cgb_tests(void) {
    uint8_t *p=synthetic(0,0,0); p[0x143]=0xc0; assert(!gb_open(p,32768,1000));
    writeb(0xff4f,1); writeb(0x8000,0x42); writeb(0xff4f,0); assert(readb(0x8000)==0);
    writeb(0xff4f,1); assert(readb(0x8000)==0x42);
    writeb(0xff70,7); writeb(0xd000,0x57); writeb(0xff70,0); assert(readb(0xd000)==0);
    writeb(0xff70,7); assert(readb(0xd000)==0x57);
    for(int i=0;i<32;i++) writeb(0xc000+i,(uint8_t)(i+1));
    writeb(0xff51,0xc0); writeb(0xff52,0); writeb(0xff53,0x1f); writeb(0xff54,0xf0); writeb(0xff55,1);
    assert(readb(0x9ff0)==1 && readb(0x8000)==17 && readb(0xff55)==255);
    writeb(0xff51,0xc0); writeb(0xff52,0); writeb(0xff53,0); writeb(0xff54,0x20); writeb(0xff55,0x80);
    hw_hdma(); assert(readb(0x8020)==1 && readb(0xff55)==255);
    writeb(0xff46,0xc0); assert(lcd.oam.mem[0]==1 && lcd.oam.mem[31]==32);
    /* Window clipping at WX 0..6 must not address before the scan buffers. */
    for(int x=0;x<7;x++) { writeb(0xff4a,0); writeb(0xff4b,x); writeb(0xff40,0xb3); assert(gb_frame(0,true,1000)); }
    gb_close(); free(p);
    puts("Game Boy CGB PASS: VRAM/WRAM banking, GDMA/HDMA/OAM, window clipping");
}
static void sound_tests(void) {
    for(unsigned ch=0;ch<4;ch++) {
        uint8_t *p=synthetic(0,0,0);
        p[0x100]=0xc3; p[0x101]=0x50; p[0x102]=1;
        p[0x150]=0xc3; p[0x151]=0x50; p[0x152]=1;
        assert(!gb_open(p,32768,1000));
        writeb(0xff26,0x80); writeb(0xff24,0x77); writeb(0xff25,0x11<<ch);
        if (ch==0) { writeb(0xff11,0x80); writeb(0xff12,0xf0); writeb(0xff13,0x80); writeb(0xff14,0x87); }
        if (ch==1) { writeb(0xff16,0x80); writeb(0xff17,0xf0); writeb(0xff18,0x80); writeb(0xff19,0x87); }
        if (ch==2) {
            for(int i=0;i<16;i++) writeb(0xff30+i,i*17);
            writeb(0xff1a,0x80); writeb(0xff1c,0x20); writeb(0xff1d,0x80); writeb(0xff1e,0x87);
        }
        if (ch==3) { writeb(0xff21,0xf0); writeb(0xff22,0x35); writeb(0xff23,0x80); }
        unsigned before=nonzero;
        for(int f=0;f<300;f++) assert(gb_frame(0,false,1000+f/60));
        assert(nonzero>before+1000);
        gb_close(); free(p);
    }
    puts("Game Boy sound PASS: both pulse channels, wave and noise independently produce PCM");
}
int main(int argc,char **argv) {
    assert(argc==3);
    cartridge_tests(); clock_tests(); cgb_tests(); sound_tests();
    for(int mode=0;mode<2;mode++) {
        size_t size; uint8_t *p=readrom(argv[1+mode],&size);
        assert(!gb_open(p,size,1000));
        assert(gb_cartridge.cgb==mode);
        unsigned initial=gb_hash(gb_pixels(),160*144*2);
        uint32_t source_hash=gb_hash(p,size);
        for(int f=0;f<120;f++) assert(gb_frame(f<60 ? GB_RIGHT|GB_A : GB_LEFT|GB_B,true,1000+f/60));
        assert(source_hash==gb_hash(p,size));
        assert(samples && nonzero); assert(initial!=gb_hash(gb_pixels(),160*144*2));
        assert(gb_ram()[1]&2); assert(gb_ram()[2]&2); assert(gb_ram()[3]>50);
        assert(cpu.speed==mode);
        uint8_t x=gb_ram()[0]; gb_reset(); assert(gb_ram()[0]==x);
        gb_close(); free(p);
    }
    printf("Game Boy PASS: DMG/CGB 240 frames, %u samples (%u nonzero), controls, SRAM reset\n",samples,nonzero);
    return 0;
}
