/* Cartridge-only Glaze adapter; bus layout follows Beetle PCE Fast's libretro
 * HuC load path (GPL-2.0-or-later). UI integration is GPLv3. */
#include "../../lib/sega/core.h"
#include "../../third_party/beetle-pce-fast/pce_fast/pce.h"
#include "../../third_party/beetle-pce-fast/pce_fast/huc.h"
#include "../../third_party/beetle-pce-fast/pce_fast/huc6280.h"
#include "../../third_party/beetle-pce-fast/pce_fast/vdc.h"
#include "../../third_party/beetle-pce-fast/pce_fast/psg.h"
#include <stdlib.h>
#include <string.h>

struct sega_cart sega_cartridge;
uint8 ROMSpace[0x88*8192+8192],BaseRAM[32768+8192],PCEIODataBuffer;
bool PCE_ACEnabled,OrderOfGriffonFix;
static uint8 save_ram[2048];
static uint16 pixels[512*240],picture[512*240];
static unsigned width=256,buttons,select_line,read_index;
static PCEFast_PSG *psg;
static Blip_Buffer audio_buffers[2];
static bool opened,populous;

unsigned MDFN_GetSettingUI(const char *name) {
    if(!strcmp(name,"pce_fast.slend"))return 239;
    if(!strcmp(name,"pce_fast.hoverscan"))return 352;
    return 0;
}
bool MDFN_GetSettingB(const char *name) { (void)name;return false; }
static DECLFR(bus_read) { (void)A;return 0xff; }
static DECLFW(bus_write) { (void)A;(void)V; }
static DECLFR(rom_read) { return ROMSpace[A]; }
static DECLFW(cart_ram_write) { ROMSpace[A]=V; }
static DECLFR(ram_read) { return BaseRAM[A&8191]; }
static DECLFW(ram_write) { BaseRAM[A&8191]=V; }
static DECLFR(save_read) { return (A&8191)<2048 ? save_ram[A&2047] : 0xff; }
static DECLFW(save_write) { if((A&8191)<2048)save_ram[A&2047]=V; }
static unsigned pad_read(void) {
    unsigned pressed=0;
    if(select_line&1) {
        if(buttons&SEGA_UP)pressed|=1;
        if(buttons&SEGA_RIGHT)pressed|=2;
        if(buttons&SEGA_DOWN)pressed|=4;
        if(buttons&SEGA_LEFT)pressed|=8;
    } else {
        if(buttons&SEGA_A)pressed|=1;
        if(buttons&SEGA_B)pressed|=2;
        if(buttons&SEGA_C)pressed|=4;
        if(buttons&SEGA_START)pressed|=8;
    }
    /* No multitap: subsequent pad indices are disconnected. */
    return 0xb0|(read_index ? 0 : (15^pressed));
}
static DECLFR(io_read) {
    A&=8191;
    switch(A>>10) {
    case 0:HuC6280_StealCycle();return VDC_Read(A);
    case 1:HuC6280_StealCycle();return VCE_Read(A);
    case 2:return HuCPU.in_block_move ? 0 : PCEIODataBuffer;
    case 3:if(HuCPU.in_block_move)return 0;return PCEIODataBuffer=HuC6280_TimerRead(A);
    case 4:if(HuCPU.in_block_move)return 0;return PCEIODataBuffer=pad_read();
    case 5:if(HuCPU.in_block_move)return 0;return PCEIODataBuffer=HuC6280_IRQStatusRead(A);
    default:return 0xff;
    }
}
static DECLFW(io_write) {
    A&=8191;
    switch(A>>10) {
    case 0:HuC6280_StealCycle();VDC_Write(A,V);break;
    case 1:HuC6280_StealCycle();VCE_Write(A,V);break;
    case 2:PCEIODataBuffer=V;PCEFast_PSG_Write(psg,HuCPU.timestamp,A,V);break;
    case 3:PCEIODataBuffer=V;HuC6280_TimerWrite(A,V);break;
    case 4:
        PCEIODataBuffer=V;
        if((V&1) && !(select_line&2) && (V&2))read_index=0;
        else if((V&1) && !(select_line&1) && read_index<255)read_index++;
        select_line=V&3;break;
    case 5:PCEIODataBuffer=V;HuC6280_IRQStatusWrite(A,V);break;
    }
}
const char *sega_validate(const uint8_t *p,size_t size,struct sega_cart *c) {
    if(!p)return "Missing HuCard";
    unsigned header=size%8192==512 ? 512 : 0;
    if(size<header+8192 || size-header>1024*1024 || (size-header)%8192)
        return "HuCard must be 8 KiB–1 MiB, in 8 KiB banks";
    p+=header;size-=header;
    if((size&(size-1)) && size!=0x60000 && size!=0xc0000)
        return "HuCard size must be a power of two, 384 or 768 KiB";
    unsigned pc=p[8190]|((unsigned)p[8191]<<8);
    if(pc==0 || pc==65535)return "Invalid HuCard reset vector (use native-order .pce)";
    memset(c,0,sizeof *c);c->rom_size=size;c->hash=sega_hash(p,size);
    c->ram_size=!memcmp(p+0x1f26,"POPULOUS",8) ? 32768 : 2048;c->battery=true;
    strcpy(c->title,"PC Engine / TurboGrafx-16 HuCard");return NULL;
}
const char *sega_open(const uint8_t *p,size_t size) {
    struct sega_cart c;const char *error=sega_validate(p,size,&c);if(error)return error;
    sega_close();p+=size-c.rom_size;size=c.rom_size;sega_cartridge=c;
    populous=c.ram_size==32768;
    HuC6280_Init();pce_overclocked=1;PCE_IsCD=false;PCE_ACEnabled=false;
    memset(ROMSpace,0xff,sizeof ROMSpace);
    for(unsigned bank=0;bank<128;bank++) {
        HuCPU.FastMap[bank]=ROMSpace+bank*8192;
        unsigned offset=(bank*8192)%size;
        if(size==0x60000)offset=bank<0x40 ? ((bank&31)*8192) : (0x40000+(bank&15)*8192);
        if(size==0x80000 && bank>=0x40)offset=0x40000+(bank&31)*8192;
        if(size==0xc0000 && bank>=0x60)continue; /* Absent upper ROM remains open bus. */
        memcpy(ROMSpace+bank*8192,p+offset,8192);
    }
    for(unsigned bank=0;bank<256;bank++) {
        HuCPU.PCERead[bank]=bank<128 ? rom_read : bus_read;
        HuCPU.PCEWrite[bank]=bus_write;
    }
    if(populous) {
        memset(ROMSpace+0x80000,0xff,32768);
        for(unsigned bank=0x40;bank<0x44;bank++)HuCPU.PCEWrite[bank]=cart_ram_write;
    } else {
        memset(save_ram,0,sizeof save_ram);memcpy(save_ram,"HUBM\0\210\020\200",8);
        HuCPU.PCERead[0xf7]=save_read;HuCPU.PCEWrite[0xf7]=save_write;
    }
    for(unsigned bank=0xf8;bank<=0xfb;bank++) {
        HuCPU.FastMap[bank]=BaseRAM;HuCPU.PCERead[bank]=ram_read;HuCPU.PCEWrite[bank]=ram_write;
    }
    HuCPU.PCERead[0xff]=io_read;HuCPU.PCEWrite[0xff]=io_write;
    VDC_Init();memset(audio_buffers,0,sizeof audio_buffers);
    psg=PCEFast_PSG_new(&audio_buffers[0],&audio_buffers[1]);
    if(!vdc || !psg) { sega_close();return "Not enough memory for TurboGrafx-16"; }
    opened=true;VDC_SetPixelFormat(NULL,0);sega_reset();return NULL;
}
void sega_close(void) {
    PCEFast_PSG_free(psg);psg=NULL;VDC_Close();opened=false;
    memset(&sega_cartridge,0,sizeof sega_cartridge);
}
void sega_reset(void) {
    if(!opened)return;
    memset(BaseRAM,0,sizeof BaseRAM);memset(pixels,0,sizeof pixels);memset(picture,0,sizeof picture);
    memset(audio_buffers,0,sizeof audio_buffers);memset(&vce,0,sizeof vce);
    width=256;buttons=select_line=read_index=0;PCEIODataBuffer=0xff;
    HuC6280_Power();VDC_Power();PCEFast_PSG_Power(psg,0);
}
void sega_set_pal(bool pal) { (void)pal; } /* HuCards use NTSC hardware timing. */
bool sega_frame(unsigned input,bool render) {
    if(!opened)return false;
    buttons=input;
    MDFN_Surface surface={pixels,512};EmulateSpecStruct e={.surface=&surface,.skip=!render};
    VDC_RunFrame(&e,false);unsigned clocks=HuCPU.timestamp;
    PCEFast_PSG_EndFrame(psg,clocks);
    for(unsigned lr=0;lr<2;lr++)glaze_pce_audio_end(&audio_buffers[lr],clocks);
    if(audio_buffers[0].overflow || audio_buffers[1].overflow)return false;
    int16_t mono[1024];unsigned n=audio_buffers[0].count;
    for(unsigned i=0;i<n;i++)mono[i]=((int)audio_buffers[0].pcm[i]+audio_buffers[1].pcm[i])/2;
    sega_audio(mono,n);HuC6280_ResetTS();
    if(render) {
        if(e.DisplayRect.w<1 || e.DisplayRect.w>512)return false;
        width=e.DisplayRect.w;
        for(unsigned y=0;y<240;y++)memcpy(picture+y*width,pixels+y*512,width*2);
    }
    return true;
}
const uint16_t *sega_pixels(void) { return picture; }
unsigned sega_width(void) { return width; }
unsigned sega_height(void) { return 240; }
uint8_t *sega_ram(void) { return populous ? ROMSpace+0x80000 : save_ram; }
