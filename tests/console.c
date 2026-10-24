/* Original generated Game Gear / HuCard demos exercise CPU-facing hardware. */
#include "../lib/sega/core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned samples,nonzero,transitions;static int previous;
void sega_audio(const int16_t *p,unsigned n) {
    while(n--) { int value=*p++;nonzero+=value!=0;transitions+=value!=previous;previous=value;samples++; }
}
void sega_log(const char *s) { fprintf(stderr,"core: %s\n",s); }
static void snapshot(const char *path) {
    FILE *f=fopen(path,"wb");assert(f);
    unsigned w=sega_width(),h=sega_height();fprintf(f,"P6\n%u %u\n255\n",w,h);
    for(unsigned i=0;i<w*h;i++) {
        unsigned v=sega_pixels()[i];unsigned char rgb[]={((v>>11)&31)*255/31,((v>>5)&63)*255/63,(v&31)*255/31};
        assert(fwrite(rgb,1,3,f)==3);
    }
    assert(!fclose(f));
}
int main(int argc,char **argv) {
    assert(argc==3);FILE *f=fopen(argv[1],"rb");assert(f);
    fseek(f,0,SEEK_END);size_t size=ftell(f);rewind(f);
    uint8_t *rom=malloc(size);assert(rom && fread(rom,1,size,f)==size);fclose(f);
    struct sega_cart cart;assert(!sega_validate(rom,size,&cart));
    assert(sega_validate(NULL,size,&cart));
    for(unsigned n=0;n<512;n++)assert(sega_validate(rom,n,&cart));
    assert(sega_validate(rom,size-1,&cart));assert(sega_validate(rom,SEGA_ROM_MAX+1,&cart));
    uint32_t original=sega_hash(rom,size);
#ifdef TEST_GG
    rom[0x7fff]=0x4c;assert(sega_validate(rom,size,&cart));rom[0x7fff]=0x6c;
    rom[0x7ff0]^=1;assert(sega_validate(rom,size,&cart));rom[0x7ff0]^=1;
#else
    rom[8191]=rom[8190]=0;assert(sega_validate(rom,size,&cart));rom[8191]=0xe0;
    uint8_t *headered=malloc(size+512);assert(headered);
    memset(headered,0x67,512);memcpy(headered+512,rom,size);
    struct sega_cart with_header;assert(!sega_validate(headered,size+512,&with_header));
    assert(!sega_validate(rom,size,&cart) && with_header.hash==cart.hash);free(headered);
#endif
    const char *error=sega_open(rom,size);if(error)fprintf(stderr,"open: %s\n",error);assert(!error);
    uint32_t first=0,last=0;
    for(unsigned frame=0;frame<180;frame++) {
        unsigned keys=frame<60 ? SEGA_RIGHT|SEGA_B : SEGA_LEFT|SEGA_A|SEGA_B|SEGA_C|SEGA_START;
        assert(sega_frame(keys,true));
        if(frame==20)first=sega_hash(sega_pixels(),sega_width()*sega_height()*2);
        last=sega_hash(sega_pixels(),sega_width()*sega_height()*2);
    }
    printf("frames=180 size=%ux%u PCM=%u nonzero=%u transitions=%u pictures=%08x,%08x\n",sega_width(),sega_height(),samples,nonzero,transitions,first,last);fflush(stdout);
    snapshot(argv[2]);assert(samples>50000 && nonzero>1000 && transitions>100);
    assert(first!=last && sega_hash(rom,size)==original);
    uint8_t *ram=sega_ram();
#ifdef TEST_GG
    assert(sega_width()==160 && sega_height()==144);
    printf("GG pad=%u Start=%u frames=%u\n",ram[1],ram[4],ram[3]);fflush(stdout);
    assert((ram[1]&0x34)==0x34 && ram[4]==128 && ram[3]>50);
#else
    printf("PCE buttons=%u directions=%u frames=%u\n",ram[0x11],ram[0x12],ram[0x13]);fflush(stdout);
    assert((ram[0x11]&15)==15 && ram[0x12]==8 && ram[0x13]>50);
#endif
    uint32_t saved=sega_hash(ram,sega_cartridge.ram_size);
    sega_reset();assert(saved==sega_hash(sega_ram(),sega_cartridge.ram_size));
    sega_set_pal(true);assert(!sega_cartridge.pal);
    for(unsigned frame=0;frame<10;frame++)assert(sega_frame(0,false));
    sega_close();free(rom);puts("PASS: cartridge validation, graphics, pad/Start, PCM, reset RAM, unchanged ROM");return 0;
}
