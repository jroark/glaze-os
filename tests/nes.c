/* Execute our original cartridge against the real CPU/PPU/APU core. */
#include "../packages/nes/core.h"
#include "../third_party/infones/InfoNES.h"
#include "../third_party/infones/InfoNES_Mapper.h"
#include "../third_party/infones/K6502.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern WORD PC;
static unsigned frames,video_frames,audio_samples,nonzero_samples,input;
static uint32_t first_picture,last_picture;
void nes_log(const char *s) { fputs(s,stderr); }
void nes_audio_open(unsigned hz) { assert(hz==22050); }
void nes_audio_close(void) {}
void nes_audio(const int16_t *pcm,unsigned n)
{
    assert(n>0 && n<=735); audio_samples+=n;
    for (unsigned i=0;i<n;i++) nonzero_samples+=pcm[i]!=0;
}
void nes_video(const uint16_t *pixels)
{
    uint32_t h=nes_hash(pixels,256*240*2);
    if (video_frames==10) first_picture=h;
    last_picture=h; video_frames++;
}
unsigned nes_poll(void) { return ++frames>=120 ? 256 : input; }
static uint8_t *readrom(const char *path,size_t *size)
{
    FILE *f=fopen(path,"rb"); assert(f); assert(!fseek(f,0,SEEK_END));
    long n=ftell(f);assert(n>0);*size=n;rewind(f);
    uint8_t *p=malloc(n);assert(p);assert(fread(p,1,n,f)==(size_t)n);fclose(f);return p;
}
static void validation(uint8_t *rom,size_t size)
{
    struct nes_cart c;
    assert(!nes_validate(rom,size,&c)); assert(c.battery && c.mapper==0);
    for (size_t n=0;n<16;n++) assert(nes_validate(rom,n,&c));
    assert(nes_validate(rom,size-1,&c));assert(nes_validate(rom,size+1,&c));
    uint8_t h[16];memcpy(h,rom,16);
    rom[4]=0;assert(nes_validate(rom,size,&c));memcpy(rom,h,16);
    rom[7]=0x0c;assert(nes_validate(rom,size,&c));memcpy(rom,h,16);
    rom[6]=0x50;assert(nes_validate(rom,size,&c));memcpy(rom,h,16);
    rom[9]=1;assert(nes_validate(rom,size,&c));memcpy(rom,h,16);
    rom[6]=0x30;rom[5]=0;assert(nes_validate(rom,size,&c));memcpy(rom,h,16);
    assert(nes_rgb565(0)==0 && nes_rgb565(0x7fff)==0xffff && nes_rgb565(0xffff)==0xffff);
}
/* Header-only fixtures reuse our original cartridge; no commercial ROM is bundled. */
static void nes2_validation(const uint8_t *legacy,size_t size)
{
    uint8_t *rom=malloc(size+512);assert(rom);
    memcpy(rom,legacy,size);
    rom[7]=8;rom[10]=0x70;rom[12]=2;rom[15]=1;
    uint8_t header[16];memcpy(header,rom,16);
    struct nes_cart c;
    assert(!nes_validate(rom,size,&c));
    assert(c.nes2 && c.mapper==0 && c.submapper==0 && c.battery);
    assert(c.prg_size==16384 && c.chr_size==8192 && c.prg_nvram_size==8192);
    assert(!c.prg_ram_size && !c.chr_ram_size);

    /* Both ROM-size encodings must configure the same unmodified payload. */
    rom[4]=14*4;rom[5]=13*4;rom[9]=0xff;
    assert(!nes_open(rom,size));
    assert(NesHeader.byRomSize==1 && NesHeader.byVRomSize==1);
    assert(!NesHeader.byInfo2 && MapperNo==0);
    for (unsigned i=0;i<sizeof NesHeader.byReserve;i++) assert(!NesHeader.byReserve[i]);
    assert(!memcmp(ROM,legacy+16,16384) && !memcmp(VROM,legacy+16+16384,8192));
    assert(rom[4]==14*4 && rom[5]==13*4 && rom[9]==0xff && rom[12]==2);
    nes_close();
    rom[4]=1;rom[9]=0xf0;assert(!nes_validate(rom,size,&c));
    rom[5]=1;rom[4]=14*4;rom[9]=15;assert(!nes_validate(rom,size,&c));
    rom[4]=14*4+1; /* Three 16 KiB units: decodable, but this core needs powers of two. */
    assert(nes_validate(rom,size,&c));
    rom[4]=0xfc;assert(nes_validate(rom,size,&c)); /* exponent 63: no oversized shift */
    memcpy(rom,header,16);rom[9]=1;assert(nes_validate(rom,size,&c));
    memcpy(rom,header,16);rom[9]=0x10;assert(nes_validate(rom,size,&c));

    static const struct { unsigned byte, value; } invalid[] = {
        {8,1}, {8,0x10}, /* extended mapper and submapper must never be truncated */
        {7,9}, {7,11}, {12,1}, {12,3}, {12,4}, /* console/timing/reserved */
        {13,1}, {14,1}, {14,4}, {15,2}, {15,0x80},
        {6,0}, {10,0}, {10,0x60}, {10,0xf0}, {10,0x77}, /* battery/RAM */
        {11,7}, {11,0x70} /* hybrid CHR and nonvolatile CHR RAM */
    };
    for (unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++) {
        memcpy(rom,header,16);rom[invalid[i].byte]=invalid[i].value;
        assert(nes_validate(rom,size,&c));
    }
    memcpy(rom,header,16);rom[6]&=~2;rom[10]=7;
    assert(!nes_validate(rom,size,&c) && !c.battery && c.prg_ram_size==8192);
    rom[10]=0;assert(!nes_validate(rom,size,&c) && !c.prg_ram_size);
    assert(nes_validate(rom,size-1,&c) && nes_validate(rom,size+1,&c));

    memcpy(rom,header,16);rom[5]=0;rom[11]=7;
    assert(!nes_validate(rom,size-8192,&c) && c.chr_ram_size==8192);
    assert(!nes_open(rom,size-8192));assert(!VROM && nes_cartridge.chr_ram_size==8192);nes_close();
    rom[11]=0;assert(nes_validate(rom,size-8192,&c));
    rom[11]=8;assert(nes_validate(rom,size-8192,&c));

    memcpy(rom,header,16);rom[6]|=4;
    memset(rom+16,0x5a,512);memcpy(rom+528,legacy+16,size-16);
    assert(!nes_validate(rom,size+512,&c) && c.prg_offset==528 && c.trainer);
    assert(!nes_open(rom,size+512));
    assert(nes_sram()[0x1000]==0x5a && nes_sram()[0x11ff]==0x5a);
    assert(!memcmp(ROM,legacy+16,16384));nes_close();
    free(rom);
    puts("nes: PASS NES 2.0 metadata, sizes, RAM, trainers and core configuration");
}

static void banking(void)
{
    for (unsigned mapper=0;mapper<=4;mapper++) {
        size_t n=16+65536+32768;
        uint8_t *r=calloc(1,n);assert(r);memcpy(r,"NES\032",4);r[4]=4;r[5]=4;r[6]=mapper<<4;
        if (mapper==0 || mapper==3) {r[4]=2;n-=32768;}
        if (mapper==0) {r[5]=1;n-=24576;}
        if (mapper==2) {r[5]=0;n-=32768;}
        for (unsigned j=0;j<r[4]*2u;j++) memset(r+16+j*8192,j,8192);
        assert(!nes_open(r,n));assert(nes_cartridge.mapper==mapper);
        assert(ROMBANK0>=ROM && ROMBANK0<ROM+nes_cartridge.prg_size);
        if (mapper==2) {MapperWrite(0x8000,1);assert(*ROMBANK0==2);}
        if (mapper==1) {
            MapperWrite(0x8000,0x80);
            for (int bit=0;bit<5;bit++) MapperWrite(0xe000,(1>>bit)&1);
            assert(*ROMBANK0==2);
        }
        if (mapper==3) {MapperWrite(0x8000,3);assert(PPUBANK[0]==VROM+3*8192);}
        if (mapper==4) {MapperWrite(0x8000,6);MapperWrite(0x8001,3);assert(*ROMBANK0==3);}
        r[7]=8;r[11]=r[5] ? 0 : 7;r[12]=2;r[15]=1;
        assert(!nes_open(r,n) && nes_cartridge.nes2 && MapperNo==mapper);
        nes_close();free(r);
    }
}
int main(int argc,char **argv)
{
    assert(argc==2);size_t size;uint8_t *rom=readrom(argv[1],&size);validation(rom,size);nes2_validation(rom,size);banking();
    rom[7]=8;rom[10]=0x70;rom[12]=2;rom[15]=1;
    assert(!nes_open(rom,size));input=NES_RIGHT|NES_A;nes_run();
    fprintf(stderr,"metrics frames=%u video=%u audio=%u nonzero=%u sram=%u,%u PC=%04x\n",frames,video_frames,audio_samples,nonzero_samples,nes_sram()[0],nes_sram()[1],PC);
    assert(frames==120 && video_frames>=50 && audio_samples>40000 && nonzero_samples>0);
    assert(nes_sram()[0]==(NES_RIGHT|NES_A));assert(nes_sram()[1]>50);
    assert(last_picture!=first_picture); /* Sprite moves and changes palette. */
    printf("nes: PASS CPU/PPU/APU/controller, %u frames, %u audio samples, %u nonzero\n",frames,audio_samples,nonzero_samples);
    nes_reset();assert(nes_sram()[0]==(NES_RIGHT|NES_A));
    nes_close();free(rom);puts("nes: PASS loader, mappers 0-4, RGB, controls and SRAM reset persistence");
    return 0;
}
