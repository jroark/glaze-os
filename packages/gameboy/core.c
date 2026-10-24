#include "gameboy.h"
#include "../../third_party/gnuboy/defs.h"
#include "../../third_party/gnuboy/cpu.h"
#include "../../third_party/gnuboy/hw.h"
#include "../../third_party/gnuboy/mem.h"
#include "../../third_party/gnuboy/regs.h"
#include "../../third_party/gnuboy/lcd.h"
#include "../../third_party/gnuboy/rtc.h"
#include "../../third_party/gnuboy/sound.h"
#include "../../third_party/gnuboy/pcm.h"
#include "../../third_party/gnuboy/fb.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct gb_cart gb_cartridge;
struct fb fb;
struct pcm pcm;
static uint16_t pixels[160*144];
static byte pcm_buffer[512];
static bool faulted;
static int cycle_debt, previous_sample, filtered_sample;
extern void gb_clock_init(uint32_t wall);
void die(char *fmt,...)
{
    char message[160]; va_list ap;
    va_start(ap,fmt); vsnprintf(message,sizeof message,fmt,ap); va_end(ap);
    gb_log(message); faulted=true;
}
int pcm_submit(void)
{
    int16_t samples[512];
    for(int i=0;i<pcm.pos;i++) {
        int value=((int)pcm.buf[i]-128)*256;
        filtered_sample=value-previous_sample+filtered_sample*255/256;
        previous_sample=value;
        if (filtered_sample>32767) filtered_sample=32767;
        if (filtered_sample< -32768) filtered_sample= -32768;
        samples[i]=filtered_sample;
    }
    if (pcm.pos) gb_audio(samples,(unsigned)pcm.pos);
    pcm.pos=0;
    return 1;
}
const uint16_t *gb_pixels(void) { return pixels; }
uint8_t *gb_ram(void) { return (uint8_t *)ram.sbank; }
void gb_close(void)
{
    free(ram.sbank); ram.sbank=NULL; rom.bank=NULL;
    memset(&gb_cartridge,0,sizeof gb_cartridge);
    memset(&mbc,0,sizeof mbc); memset(&rtc,0,sizeof rtc);
}
void gb_reset(void)
{
    if (!rom.bank) return;
    memset(&cpu,0,sizeof cpu); memset(&scan,0,sizeof scan);
    memset(ram.ibank,0,sizeof ram.ibank); memset(pixels,0,sizeof pixels);
    hw.hdma=0;
    hw_reset(); lcd_reset(); cpu_reset(); mbc_reset(); sound_reset();
    REG(RI_BOOT)=0xff;
    mem_updatemap();
    cycle_debt=previous_sample=filtered_sample=0; pcm.pos=0; faulted=false;
}
const char *gb_open(const uint8_t *data,size_t size,uint32_t wall)
{
    struct gb_cart cart;
    const char *error=gb_validate(data,size,&cart);
    if (error) return error;
    uint8_t *sram=cart.ram_size ? calloc(1,cart.ram_size<8192 ? 8192 : cart.ram_size) : NULL;
    if (cart.ram_size && !sram) return "Not enough memory for cartridge RAM";
    gb_close(); gb_cartridge=cart;
    rom.bank=(byte (*)[16384])(void *)data; /* core never writes cartridge storage */
    ram.sbank=(byte (*)[8192])sram;
    mbc.type=cart.mapper; mbc.batt=cart.battery;
    mbc.romsize=cart.rom_size/16384;
    mbc.ram_bytes=cart.ram_size; mbc.ramsize=(cart.ram_size+8191)/8192;
    rtc.batt=cart.clock; gb_clock_init(wall);
    hw.cgb=cart.cgb; hw.gba=0;
    memset(&fb,0,sizeof fb); fb.ptr=(byte *)pixels;
    fb.w=160; fb.h=144; fb.pitch=320; fb.pelsize=2; fb.enabled=1;
    fb.cc[0].l=11; fb.cc[0].r=3;
    fb.cc[1].l=5; fb.cc[1].r=2;
    fb.cc[2].l=0; fb.cc[2].r=3;
    pcm.hz=22050; pcm.stereo=0; pcm.buf=pcm_buffer; pcm.len=sizeof pcm_buffer;
    gb_reset();
    return NULL;
}
bool gb_frame(unsigned keys,bool render,uint32_t wall)
{
    if (!rom.bank || faulted) return false;
    gb_clock_sync(wall);
    hw.pad=keys&255; pad_refresh();
    fb.enabled=render;
    /* Gnuboy counts at 2,097,152 Hz. Carry instruction overshoot between frames. */
    int left=35112-cycle_debt;
    while (left>0 && !faulted) {
        int batch=left>228 ? 228 : left;
        int ran=cpu_emulate(batch);
        if (ran<=0) { die("CPU failed to advance"); break; }
        left-=ran;
    }
    cycle_debt= -left;
    sound_mix(); pcm_submit();
    if (!(R_LCDC&128)) memset(pixels,0xff,sizeof pixels);
    return !faulted;
}
