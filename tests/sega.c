/* Both adapters execute original generated cartridges with real controller reads. */
#include "../lib/sega/core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned samples,nonzero;
void sega_audio(const int16_t *p,unsigned n) { samples+=n; while(n--) nonzero+=*p++!=0; }
void sega_log(const char *s) { fprintf(stderr,"core: %s\n",s); }
static uint8_t *readrom(const char *path,size_t *size) {
    FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);*size=ftell(f);rewind(f);
    uint8_t *p=malloc(*size);assert(p);assert(fread(p,1,*size,f)==*size);fclose(f);return p;
}
static void snapshot(const char *path) {
    FILE *f=fopen(path,"wb");assert(f);unsigned w=sega_width(),h=sega_height();
    fprintf(f,"P6\n%u %u\n255\n",w,h);const uint16_t *px=sega_pixels();
    for(unsigned i=0;i<w*h;i++) { unsigned v=px[i];uint8_t rgb[]={((v>>11)&31)*255/31,((v>>5)&63)*255/63,(v&31)*255/31};fwrite(rgb,1,3,f); }
    fclose(f);
}
int main(int argc,char **argv) {
    assert(argc==3);size_t size;uint8_t *rom=readrom(argv[1],&size);struct sega_cart cart;
    assert(!sega_validate(rom,size,&cart));
    for(unsigned n=0;n<512;n++) assert(sega_validate(rom,n,&cart));
    assert(sega_validate(rom,size-1,&cart));
    assert(sega_validate(rom,SEGA_ROM_MAX+1,&cart));
    uint32_t original=sega_hash(rom,size);
#ifdef TEST_GENESIS
    rom[0x100]^=1; assert(sega_validate(rom,size,&cart));rom[0x100]^=1;
    uint8_t old=rom[7];rom[7]|=1;assert(sega_validate(rom,size,&cart));rom[7]=old;
    old=rom[0x1b3];rom[0x1b3]=0x40;assert(sega_validate(rom,size,&cart));rom[0x1b3]=old;
#else
    rom[0x7ff0]^=1;assert(sega_validate(rom,size,&cart));rom[0x7ff0]^=1;
    rom[0x7fff]=0x5c;assert(sega_validate(rom,size,&cart));rom[0x7fff]=0x4c;
#endif
    const char *error=sega_open(rom,size);if(error) fprintf(stderr,"open: %s\n",error);assert(!error);
    uint32_t first=0,last=0;
    for(unsigned f=0;f<180;f++) {
        unsigned input=f<60 ? SEGA_RIGHT|SEGA_B : SEGA_LEFT|SEGA_A|SEGA_B|SEGA_C|SEGA_START;
        assert(sega_frame(input,true));
        if(f==20) first=sega_hash(sega_pixels(),sega_width()*sega_height()*2);
        last=sega_hash(sega_pixels(),sega_width()*sega_height()*2);
    }
    printf("frames=180 PCM=%u nonzero=%u pictures=%08x,%08x SRAM=%u,%u,%u,%u,%u,%u\n",samples,nonzero,first,last,sega_ram()[0],sega_ram()[1],sega_ram()[2],sega_ram()[3],sega_ram()[4],sega_ram()[5]);
    fflush(stdout); snapshot(argv[2]);
    assert(samples>50000 && nonzero>1000);assert(first!=last);
#ifdef TEST_GENESIS
    assert((sega_ram()[1]&0x34)==0x34);assert((sega_ram()[3]&0x30)==0x30);assert(sega_ram()[5]>50);
#else
    assert((sega_ram()[1]&0x34)==0x34);assert(sega_ram()[3]>50);assert(sega_ram()[4]>0);
#endif
    assert(original==sega_hash(rom,size));snapshot(argv[2]);
    uint32_t ramhash=sega_hash(sega_ram(),sega_cartridge.ram_size);sega_reset();assert(ramhash==sega_hash(sega_ram(),sega_cartridge.ram_size));
    sega_set_pal(true);assert(sega_cartridge.pal);
    assert(ramhash==sega_hash(sega_ram(),sega_cartridge.ram_size));
    for(int i=0;i<5;i++) assert(sega_frame(0,false));
    sega_close();free(rom);puts("Sega PASS: validation, CPU/VDP, input, audio, RAM reset, PAL execution, unchanged ROM");return 0;
}
