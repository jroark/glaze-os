/* Optional external-cartridge smoke runner. ROMs remain outside Git. */
#include "../lib/sega/core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned long samples,nonzero,audio_changes;
static int previous_sample,minimum=32767,maximum=-32768;
void sega_audio(const int16_t *p,unsigned n) {
    while(n--) {
        int value=*p++;
        if(samples && value!=previous_sample)audio_changes++;
        nonzero+=value!=0;samples++;previous_sample=value;
        if(value<minimum)minimum=value;
        if(value>maximum)maximum=value;
    }
}
void sega_log(const char *s) { fprintf(stderr,"core: %s\n",s); }
static int snapshot(const char *prefix,unsigned frame) {
    char path[1024];
    if(snprintf(path,sizeof path,"%s-%u.ppm",prefix,frame)>=(int)sizeof path) return -1;
    FILE *f=fopen(path,"wb");if(!f)return -1;
    unsigned w=sega_width(),h=sega_height();const uint16_t *p=sega_pixels();
    fprintf(f,"P6\n%u %u\n255\n",w,h);
    for(unsigned i=0;i<w*h;i++) {
        unsigned v=p[i];unsigned char rgb[]={((v>>11)&31)*255/31,((v>>5)&63)*255/63,(v&31)*255/31};
        if(fwrite(rgb,1,3,f)!=3) { fclose(f);return -1; }
    }
    return fclose(f);
}
int main(int argc,char **argv) {
    if(argc!=3) { fprintf(stderr,"usage: sega-rom ROM screenshot-prefix\n");return 2; }
    FILE *f=fopen(argv[1],"rb");if(!f)return 2;
    if(fseek(f,0,SEEK_END)) { fclose(f);return 2; }
    long size=ftell(f);rewind(f);
    if(size<=0 || size>SEGA_ROM_MAX) { fclose(f);return 2; }
    uint8_t *rom=malloc(size);
    if(!rom || fread(rom,1,size,f)!=(size_t)size) { fclose(f);free(rom);return 2; }
    fclose(f);uint32_t original=sega_hash(rom,size);
    const char *error=sega_open(rom,size);
    if(error) { fprintf(stderr,"open: %s\n",error);free(rom);return 1; }
    printf("ROM: %s; bytes=%ld RAM=%u PAL=%d title=%.48s\n",argv[1],size,sega_cartridge.ram_size,sega_cartridge.pal,sega_cartridge.title);fflush(stdout);
    unsigned changes=0;uint32_t previous=0;int result=1;
    for(unsigned frame=0;frame<3600;frame++) {
        unsigned keys=frame>=600 && frame<603 ? SEGA_START : 0;
        if(frame>=660 && frame<663) keys|=SEGA_A;
        if(frame>=900) {
            keys|=(frame/600)&1 ? SEGA_LEFT : SEGA_RIGHT;
            if(frame%120<20) keys|=SEGA_B;
        }
        if(!sega_frame(keys,true)) { fprintf(stderr,"CPU failed at frame %u\n",frame);goto done; }
        uint32_t picture=sega_hash(sega_pixels(),sega_width()*sega_height()*2);
        if(frame && picture!=previous)changes++;
        previous=picture;
        if(frame==119 || frame==539 || frame==899 || frame==1799 || frame==3599) {
            printf("frame=%u picture=%08x size=%ux%u PCM=%lu nonzero=%lu\n",frame+1,picture,sega_width(),sega_height(),samples,nonzero);fflush(stdout);
            if(snapshot(argv[2],frame+1))goto done;
        }
    }
    if(changes<60 || nonzero<1000 || audio_changes<1000 || maximum-minimum<64 || sega_hash(rom,size)!=original) {
        fprintf(stderr,"Insufficient picture/audio activity or modified ROM\n");goto done;
    }
    uint32_t ram=sega_hash(sega_ram(),sega_cartridge.ram_size);
    sega_reset();
    if(ram!=sega_hash(sega_ram(),sega_cartridge.ram_size)) { fprintf(stderr,"Reset lost RAM\n");goto done; }
    for(unsigned i=0;i<120;i++)if(!sega_frame(0,false))goto done;
    printf("PASS: 3600 scripted + 120 reset frames, %u picture changes, %lu PCM samples (%lu nonzero, %lu transitions, range %d..%d), unchanged ROM, reset/RAM retention; gameplay needs visual inspection\n",changes,samples,nonzero,audio_changes,minimum,maximum);
    result=0;
done:
    sega_close();free(rom);return result;
}
