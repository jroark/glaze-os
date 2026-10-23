/* Optional external Blargg serial-output oracle. Test ROMs are not vendored. */
#include "../packages/gameboy/gameboy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char output[8192];
static size_t used;
void gb_audio(const int16_t *p,unsigned n) { (void)p; (void)n; }
void gb_serial(uint8_t v) { if (used+1<sizeof output) { output[used++]=v; output[used]=0; } }
void gb_log(const char *s) { fprintf(stderr,"core: %s\n",s); }
int main(int argc,char **argv) {
    if (argc!=2) { fprintf(stderr,"usage: gameboy-rom ROM.gb\n"); return 2; }
    FILE *f=fopen(argv[1],"rb"); if (!f) return 2;
    fseek(f,0,SEEK_END); long size=ftell(f); rewind(f);
    uint8_t *p=malloc(size); if (!p || fread(p,1,size,f)!=(size_t)size) return 2;
    fclose(f);
    const char *error=gb_open(p,size,1000);
    if (error) { fprintf(stderr,"%s\n",error); free(p); return 1; }
    int result=1;
    for(unsigned frame=0;frame<60000;frame++) {
        if (!gb_frame(0,false,1000+frame/60)) break;
        if (strstr(output,"Passed") || strstr(output,"All tests passed")) { result=0; break; }
        if (strstr(output,"Failed") || strstr(output,"failed")) break;
    }
    printf("%s\n%s: %s\n",output,argv[1],result ? "FAIL" : "PASS");
    gb_close(); free(p); return result;
}
