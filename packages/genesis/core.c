#include "../../lib/sega/core.h"
#include "../../third_party/clownmdemu/source/clownmdemu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct sega_cart sega_cartridge;
static ClownMDEmu *core;
static uint16_t *rom_words;
static uint16_t pixels[320*240],palette[256];
static int16_t fm_samples[4096],psg_samples[8192];
static unsigned fm_count,psg_count,buttons,width=320,height=224,audio_phase;
static bool drawing,faulted;
static unsigned log_count;
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
const char *sega_validate(const uint8_t *p,size_t n,struct sega_cart *c) {
    if(!p || n<512 || n>SEGA_ROM_MAX || (n&1)) return "Genesis ROM must be 512 bytes–4 MiB, even size";
    if(memcmp(p+256,"SEGA",4)) return "Not a raw Genesis .md / .gen / .bin ROM";
    uint32_t pc=be32(p+4),sp=be32(p);
    if((pc&1) || pc<8 || pc>=n || (sp&1) || sp<0xff0000 || sp>0x1000000) return "Invalid Genesis reset vectors";
    memset(c,0,sizeof *c); c->hash=sega_hash(p,n); c->rom_size=n;
    for(unsigned i=0;i<48;i++) c->title[i]=p[0x150+i]>=32 && p[0x150+i]<127 ? p[0x150+i] : ' ';
    bool europe=false,other=false;
    for(unsigned i=0x1f0;i<0x200;i++) { if(p[i]=='E') europe=true; if(p[i]=='U'||p[i]=='J') other=true; }
    c->pal=europe&&!other;
    if(p[0x1b0]=='R' && p[0x1b1]=='A') {
        unsigned meta=((unsigned)p[0x1b2]<<8)|p[0x1b3];
        uint32_t start=be32(p+0x1b4),end=be32(p+0x1b8);
        if((meta&0xA71F)!=0xA000 || ((meta>>5)&7)!=1 || ((meta>>11)&3)==1) return "Genesis EEPROM / unsupported save hardware";
        if(start!=0x200000 && start!=0x200001) return "Unsupported Genesis SRAM start address";
        if(end<start || end>=0x210000) return "Genesis SRAM exceeds 64 KiB window";
        uint32_t need=end-0x200000+1,sz=2;
        while(sz<need) sz<<=1;
        c->ram_size=sz; c->battery=!!(meta&0x4000);
    }
    return NULL;
}
static void colour(void *u,cc_u16f i,cc_u16f v) {
    (void)u; if(i>=256) return;
    unsigned r=v&15,g=(v>>4)&15,b=(v>>8)&15;
    palette[i]=((r*31/15)<<11)|((g*63/15)<<5)|(b*31/15);
}
static void scanline(void *u,cc_u16f line,const cc_u8l *p,cc_u16f left,cc_u16f right,cc_u16f w,cc_u16f h) {
    (void)u; (void)left; (void)right;
    if(w>320 || h>240 || line>=h) return;
    width=w; height=h;
    if(drawing) for(unsigned x=0;x<w;x++) pixels[line*width+x]=palette[p[x]];
}
static cc_bool input(void *u,cc_u8f player,ClownMDEmu_Button button) {
    (void)u; if(player) return false;
    static const unsigned map[]={SEGA_UP,SEGA_DOWN,SEGA_LEFT,SEGA_RIGHT,SEGA_A,SEGA_B,SEGA_C,0,0,0,SEGA_START,0};
    return button<sizeof map/sizeof map[0] && (buttons&map[button]);
}
static void fm_audio(void *u,ClownMDEmu *md,size_t n,void (*generate)(ClownMDEmu *,cc_s16l *,size_t)) {
    (void)u;
    if(n>2048-fm_count) { faulted=true; return; }
    generate(md,fm_samples+fm_count*2,n); fm_count+=n;
}
static void psg_audio(void *u,ClownMDEmu *md,size_t n,void (*generate)(ClownMDEmu *,cc_s16l *,size_t)) {
    (void)u;
    if(n>8192-psg_count) { faulted=true; return; }
    generate(md,psg_samples+psg_count,n); psg_count+=n;
}
static void log_message(void *u,const char *fmt,va_list args) {
    (void)u; if(log_count++<8) { char s[192]; vsnprintf(s,sizeof s,fmt,args); sega_log(s); }
}
static void cpu_error(void *u,const char *fmt,va_list args) { faulted=true; log_message(u,fmt,args); }
static const ClownMDEmu_Callbacks callbacks={.colour_updated=colour,.scanline_rendered=scanline,.input_requested=input,.fm_audio_to_be_generated=fm_audio,.psg_audio_to_be_generated=psg_audio};
const char *sega_open(const uint8_t *p,size_t n) {
    struct sega_cart c; const char *error=sega_validate(p,n,&c); if(error) return error;
    sega_close(); core=calloc(1,sizeof *core); rom_words=malloc(n);
    if(!core || !rom_words) { sega_close(); return "Not enough memory for Genesis"; }
    for(size_t i=0;i<n/2;i++) rom_words[i]=((uint16_t)p[i*2]<<8)|p[i*2+1];
    sega_cartridge=c; log_count=0; faulted=false;
    ClownMDEmu_InitialConfiguration config={0};
    config.general.region=CLOWNMDEMU_REGION_OVERSEAS;
    config.general.tv_standard=c.pal ? CLOWNMDEMU_TV_STANDARD_PAL : CLOWNMDEMU_TV_STANDARD_NTSC;
    config.general.low_pass_filter_disabled=true;
    ClownMDEmu_SetLogCallback(log_message,NULL); Clown68000_SetErrorCallback(cpu_error,NULL);
    ClownMDEmu_Constant_Initialise(); ClownMDEmu_Initialise(core,&config,&callbacks);
    ClownMDEmu_SetCartridge(core,rom_words,n/2); sega_reset();
    return NULL;
}
void sega_close(void) { free(core); core=NULL; free(rom_words); rom_words=NULL; memset(&sega_cartridge,0,sizeof sega_cartridge); }
void sega_reset(void) {
    if(!core) return;
    faulted=false; audio_phase=0; buttons=0; memset(pixels,0,sizeof pixels);
    ClownMDEmu_HardReset(core,true,false);
}
void sega_set_pal(bool pal) { if(core) { sega_cartridge.pal=pal; core->configuration.tv_standard=pal ? CLOWNMDEMU_TV_STANDARD_PAL : CLOWNMDEMU_TV_STANDARD_NTSC; sega_reset(); } }
bool sega_frame(unsigned keys,bool render) {
    if(!core || faulted) return false;
    buttons=keys; drawing=render; fm_count=psg_count=0;
    ClownMDEmu_Iterate(core);
    if(faulted) return false;
    int16_t out[441]; unsigned fps=sega_cartridge.pal ? 50 : 60;
    audio_phase+=22050; unsigned n=audio_phase/fps; audio_phase%=fps;
    for(unsigned i=0;i<n;i++) {
        unsigned fi=fm_count ? i*fm_count/n : 0,pi=psg_count ? i*psg_count/n : 0;
        int sample=(fm_count ? ((int)fm_samples[fi*2]+fm_samples[fi*2+1])/2 : 0)+(psg_count ? psg_samples[pi]/8 : 0);
        if(sample>32767) sample=32767;
        if(sample< -32768) sample= -32768;
        out[i]=sample;
    }
    sega_audio(out,n); return true;
}
const uint16_t *sega_pixels(void) { return pixels; }
unsigned sega_width(void) { return width; }
unsigned sega_height(void) { return height; }
uint8_t *sega_ram(void) { return core ? core->state.external_ram.buffer : NULL; }
