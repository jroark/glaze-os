#include "../lib/sega/core.h"
#include "../third_party/beetle-pce-fast/pce_fast/pce.h"
#include "../third_party/beetle-pce-fast/pce_fast/huc6280.h"
#include "../third_party/beetle-pce-fast/pce_fast/vdc.h"
#include "../third_party/beetle-pce-fast/pce_fast/psg.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void sega_audio(const int16_t *p,unsigned n) { (void)p;(void)n; }
void sega_log(const char *s) { fprintf(stderr,"core: %s\n",s); }
static void sound(unsigned channel,bool noise,bool dda) {
    Blip_Buffer *b=calloc(2,sizeof *b);assert(b);
    PCEFast_PSG *p=PCEFast_PSG_new(&b[0],&b[1]);assert(p);
    PCEFast_PSG_Write(p,0,0,channel);PCEFast_PSG_Write(p,0,1,255);
    PCEFast_PSG_Write(p,0,5,255);PCEFast_PSG_Write(p,0,2,128);
    PCEFast_PSG_Write(p,0,3,1);
    for(unsigned i=0;i<32;i++)PCEFast_PSG_Write(p,0,6,i<16 ? 0 : 31);
    if(noise)PCEFast_PSG_Write(p,0,7,0x9e);
    PCEFast_PSG_Write(p,0,4,dda ? 0xdf : 0x9f);
    if(dda)PCEFast_PSG_Write(p,1000,6,31);
    PCEFast_PSG_EndFrame(p,119210);
    unsigned nonzero=0;
    for(unsigned lr=0;lr<2;lr++) {
        glaze_pce_audio_end(&b[lr],119210);assert(!b[lr].overflow && b[lr].count>300);
        for(unsigned i=0;i<b[lr].count;i++)nonzero+=b[lr].pcm[i]!=0;
    }
    assert(nonzero>100);
    /* Writes to unconnected channel selectors must not form invalid pointers. */
    for(unsigned ch=6;ch<8;ch++)for(unsigned reg=0;reg<10;reg++) {
        PCEFast_PSG_Write(p,0,0,ch);PCEFast_PSG_Write(p,0,reg,255);
    }
    PCEFast_PSG_free(p);free(b);
}
int main(void) {
    static const unsigned sizes[]={8192,0x20000,0x40000,0x60000,0x80000,0xc0000,0x100000};
    static const unsigned banks[]={0x10,0x20,0x3f,0x40,0x5f,0x60,0x7f};
    /* Physical bank tags expected at significant decoder boundaries. */
    static const uint8 expected[][7]={
        {0,0,0,0,0,0,0}, {0,0,15,0,15,0,15}, {16,0,31,0,31,0,31},
        {16,0,31,32,47,32,47}, {16,32,63,32,63,32,63},
        {16,32,63,64,95,255,255}, {16,32,63,64,95,96,127}
    };
    uint8_t *rom=malloc(0x100000);assert(rom);
    for(unsigned s=0;s<sizeof sizes/sizeof sizes[0];s++) {
        unsigned size=sizes[s];for(unsigned bank=0;bank<size/8192;bank++)memset(rom+bank*8192,bank,8192);
        rom[8190]=0;rom[8191]=0xe0;assert(!sega_open(rom,size));
        for(unsigned i=0;i<7;i++) {
            unsigned bank=banks[i];
            assert(HuCPU.PCERead[bank](bank*8192)==expected[s][i]);
            assert(HuCPU.FastMap[bank][0]==expected[s][i]);
        }
        HuCPU.PCEWrite[0xf8](0xf8*8192+0x123,0xa7);
        for(unsigned bank=0xf8;bank<=0xfb;bank++)assert(HuCPU.PCERead[bank](bank*8192+0x123)==0xa7);
        HuCPU.PCEWrite[0xf7](0xf7*8192+2047,0x63);assert(sega_ram()[2047]==0x63);
        assert(HuCPU.PCERead[0xf7](0xf7*8192+2048)==255);
        sega_reset();assert(sega_ram()[2047]==0x63);sega_close();
    }
    memset(rom,0,8192);rom[8191]=0xe0;memcpy(rom+0x1f26,"POPULOUS",8);
    assert(!sega_open(rom,8192) && sega_cartridge.ram_size==32768);
    for(unsigned bank=0x40;bank<=0x43;bank++)HuCPU.PCEWrite[bank](bank*8192+8191,bank);
    sega_reset();for(unsigned bank=0x40;bank<=0x43;bank++)assert(sega_ram()[(bank-0x40)*8192+8191]==bank);
    sega_close();free(rom);
    for(unsigned ch=0;ch<6;ch++)sound(ch,false,false);
    sound(4,true,false);sound(5,true,false);sound(0,false,true);
    puts("PCE hardware PASS: ROM sizes/mirrors, 384/512 KiB layouts, RAM mirrors, BRAM bounds, Populous retention, six wave channels, noise and DDA");
}
