/* Optional nestest CPU oracle; supply the externally obtained ROM and trace.
 * B is not a physical stored status bit; compare all other flags and cycles.
 */
#include "../packages/nes/core.h"
#include "../third_party/infones/K6502.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern WORD PC;
void nes_log(const char *s) { fputs(s,stderr); }
void nes_audio_open(unsigned hz) { (void)hz; }
void nes_audio_close(void) {}
void nes_audio(const int16_t *p,unsigned n) { (void)p;(void)n; }
void nes_video(const uint16_t *p) { (void)p; }
unsigned nes_poll(void) { return 256; }
extern BYTE A,X,Y,F,SP;
extern WORD g_wPassedClocks;
int main(int argc, char **argv) {
 if (argc!=3) { fprintf(stderr,"usage: nes-trace ROM LOG\n"); return 2; }
 FILE *romfile=fopen(argv[1],"rb");assert(romfile);
 assert(!fseek(romfile,0,SEEK_END)); long size=ftell(romfile);assert(size>0);rewind(romfile);
 uint8_t *rom=malloc(size);assert(rom);assert(fread(rom,1,size,romfile)==(size_t)size);fclose(romfile);
 assert(!nes_open(rom,size));
 PC=0xc000;A=X=Y=0;F=0x24;SP=0xfd;
 FILE *f=fopen(argv[2],"r");assert(f);char line[256];unsigned count=0;unsigned cycles=7;
 while(fgets(line,sizeof line,f)) {
  unsigned pc,a,x,y,p,sp;assert(sscanf(line,"%x",&pc)==1);
  char *regs=strstr(line,"A:");assert(regs);assert(sscanf(regs,"A:%x X:%x Y:%x P:%x SP:%x",&a,&x,&y,&p,&sp)==5);
  unsigned expected_cycles;assert(sscanf(strstr(line,"CYC:"),"CYC:%u",&expected_cycles)==1);
  if(cycles!=expected_cycles) { fprintf(stderr,"cycle mismatch line %u expected %u got %u\n",count+1,expected_cycles,cycles);return 1;}
  if(PC!=pc||A!=a||X!=x||Y!=y||(F&~0x10)!=(p&~0x10)||SP!=sp) {
   fprintf(stderr,"Mismatch line %u: got PC=%04x A=%02x X=%02x Y=%02x P=%02x SP=%02x\nExpected %s",count+1,PC,A,X,Y,F,SP,line);return 1;
  }
  g_wPassedClocks=0;K6502_Step(1);cycles+=g_wPassedClocks+1;count++;
 }
 assert(count==8991);
 nes_close();free(rom);fclose(f);
 printf("CPU oracle: %u instruction states and cycle counts PASS\n",count);return 0;
}
