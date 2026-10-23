#include "../../lib/sega/core.h"
#include "../../third_party/totalsms/sms.h"
#include "../../third_party/totalsms/sms_rom_database.h"
#include <stdlib.h>
#include <string.h>
struct sega_cart sega_cartridge;
static struct SMS_Core *core;
static uint16_t pixels[256*240];
static int16_t samples[1024];
static unsigned last_buttons;
static uint32_t colour(void *u,uint8_t r,uint8_t g,uint8_t b) {
    (void)u; return ((r*31/3)<<11)|((g*63/3)<<5)|(b*31/3);
}
static void audio(void *u,int16_t *p,uint32_t n) {
    (void)u; int16_t mono[512];
    if(n>512) n=512;
    for(unsigned i=0;i<n;i++) mono[i]=((int)p[i*2]+p[i*2+1])/2;
    sega_audio(mono,n);
}
const char *sega_validate(const uint8_t *rom,size_t n,struct sega_cart *c) {
    if (!rom || n<8192 || n>SEGA_ROM_MAX || (n&(n-1))) return "SMS ROM must be 8 KiB–4 MiB, power-of-two size";
    bool found=false;
    for(unsigned a=0x1ff0;a<=0x7ff0;a=(a+16)*2-16) if(a+16<=n && !memcmp(rom+a,"TMR SEGA",8)) {
        unsigned region=rom[a+15]>>4;
        if(region!=3 && region!=4) return "Not a Master System cartridge";
        found=true;
    }
    if(!found) {
        struct RomEntry entry;
        if(!rom_database_find_entry(&entry,SMS_crc32(rom,n)) || entry.sys!=SMS_System_SMS)
            return "Master System cartridge header is missing";
    }
    memset(c,0,sizeof *c); c->hash=sega_hash(rom,n); c->rom_size=n; c->ram_size=32768; c->battery=true;
    memcpy(c->title,"Master System cartridge",24);
    return NULL;
}
const char *sega_open(const uint8_t *rom,size_t n) {
    struct sega_cart c; const char *error=sega_validate(rom,n,&c); if(error) return error;
    sega_close(); core=calloc(1,sizeof *core); if(!core) return "Not enough memory for SMS";
    if(!SMS_init(core)) { SMS_quit(core); free(core); core=NULL; return "SMS initialization failed"; }
    SMS_set_colour_callback(core,colour); SMS_set_pixels(core,pixels,256,16);
    if(!SMS_set_apu_callback(core,audio,samples,512,22050) || !SMS_loadromEx(core,rom,n,SMS_System_SMS,SMS_Region_NTSC,SMS_Console_EXPORT)) {
        sega_close(); return "Unsupported Master System cartridge";
    }
    if(SMS_get_system_type(core)!=SMS_System_SMS) { sega_close(); return "Not a Master System cartridge"; }
    sega_cartridge=c; last_buttons=0; return NULL;
}
void sega_close(void) { if(core) { SMS_quit(core); free(core); core=NULL; } memset(&sega_cartridge,0,sizeof sega_cartridge); }
void sega_reset(void) {
    if(core) {
        SMS_loadromEx(core,core->cart.rom,core->cart.rom_size,SMS_System_SMS,core->region,core->console);
        last_buttons=0;
    }
}
void sega_set_pal(bool pal) {
    if(!core) return;
    const uint8_t *rom=core->cart.rom; size_t n=core->cart.rom_size;
    SMS_loadromEx(core,rom,n,SMS_System_SMS,pal ? SMS_Region_PAL : SMS_Region_NTSC,SMS_Console_EXPORT);
    sega_cartridge.pal=pal; last_buttons=0;
}
bool sega_frame(unsigned keys,bool render) {
    if(!core) return false;
    uint16_t buttons=keys&63;
    if(keys&SEGA_START) buttons|=1u<<13;
    SMS_set_buttons(core,last_buttons & ~buttons,false); SMS_set_buttons(core,buttons & ~last_buttons,true); last_buttons=buttons;
    SMS_skip_frame(core,!render); SMS_run(core,SMS_RunEndFrame); return true;
}
const uint16_t *sega_pixels(void) { return pixels; }
unsigned sega_width(void) { return 256; }
unsigned sega_height(void) {
    int x=0,y=0,w=0,h=0;
    if(core) SMS_get_pixel_region(core,&x,&y,&w,&h);
    return h>0 && h<=240 ? (unsigned)h : 192;
}
uint8_t *sega_ram(void) { return core ? &core->cart_ram.ram[0][0] : NULL; }
