#include "../../apps/wince/assets.h"
#include "../../apps/wince/pe.h"
#include "../../apps/wince/exeicon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int rd(void *arg,uint32_t off,void *p,uint32_t n){FILE *f=arg;if(fseek(f,off,SEEK_SET))return -1;return (int)fread(p,1,n,f);}
static void *file(const char *p,uint32_t *n){FILE *f=fopen(p,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);*n=(uint32_t)ftell(f);rewind(f);void *b=malloc(*n);if(fread(b,1,*n,f)!=*n)exit(2);fclose(f);return b;}
int main(int argc,char **argv){
 if(argc==5&&!strcmp(argv[1],"recover")){FILE *f=fopen(argv[2],"rb");if(!f)return 2;struct ce_nk_assets a;const char *err=ce_nk_recover(rd,f,NULL,&a);fclose(f);if(err){fprintf(stderr,"%s\n",err);return 1;}FILE *o=fopen(argv[3],"wb");fwrite(a.ordinals,1,a.ordinal_bytes,o);fclose(o);o=fopen(argv[4],"wb");fwrite(a.dictionary,1,a.dictionary_bytes,o);fclose(o);printf("%u ordinals bytes, %u dictionary bytes\n",a.ordinal_bytes,a.dictionary_bytes);ce_nk_assets_free(&a);return 0;}
 if(argc==5&&!strcmp(argv[1],"decode")){uint32_t n,dn;void *b=file(argv[2],&n),*d=file(argv[3],&dn),*out=NULL;uint32_t size;const char *err=ce_cpk_unpack(b,n,d,dn,&out,&size);free(b);free(d);if(err){fprintf(stderr,"%s\n",err);return 1;}FILE *f=fopen(argv[4],"wb");fwrite(out,1,size,f);fclose(f);free(out);return 0;}
 if(argc==5&&!strcmp(argv[1],"rom")){
  /* rom <nand> <module> <out.pe>: cached NK record -> synthesized PE, which the loader's parser accepts. */
  FILE *f=fopen(argv[2],"rb");if(!f)return 2;struct ce_nk_assets a;const char *err=ce_nk_recover(rd,f,NULL,&a);fclose(f);
  if(err){fprintf(stderr,"%s\n",err);return 1;}
  void *pe=NULL;uint32_t size=0;err=ce_rom_module_pe(a.modules,a.module_bytes,argv[3],&pe,&size);
  if(!err){struct ce_pe p;err=ce_pe_parse(&p,pe,size);if(!err&&!p.dll)err="not a DLL";
   if(!err){printf("base %08x size %x entry %x sections %u\n",p.base,p.size,p.entry,p.count);
    uint8_t *image=calloc(1,p.size);ce_pe_copy(&p,image);FILE *o=fopen(argv[4],"wb");fwrite(image,1,p.size,o);fclose(o);free(image);}}
  free(pe);ce_nk_assets_free(&a);if(err){fprintf(stderr,"%s\n",err);return 1;}return 0;}
 if(argc==5&&!strcmp(argv[1],"romdec")){
  uint32_t n,made;uint8_t *b=file(argv[2],&n);uint32_t cap=(uint32_t)strtoul(argv[4],NULL,0);uint8_t *out=malloc(cap?cap:1);
  const char *err=ce_rom_decompress(b,n,out,cap,&made);free(b);if(err){fprintf(stderr,"%s\n",err);free(out);return 1;}
  FILE *o=fopen(argv[3],"wb");fwrite(out,1,made,o);fclose(o);free(out);return 0;}
 if(argc==6&&!strcmp(argv[1],"range")){
  /* range <cpk> <dict> <offset> <length>: ce_cpk_read's bytes to stdout. */
  FILE *f=fopen(argv[2],"rb");uint32_t dn;void *d=file(argv[3],&dn);if(!f||!d)return 2;fseek(f,0,SEEK_END);uint32_t n=(uint32_t)ftell(f);
  struct ce_cpk_file c;const char *err=ce_cpk_open(&c,rd,f,n,d,dn);uint32_t off=(uint32_t)strtoul(argv[4],NULL,0),len=(uint32_t)strtoul(argv[5],NULL,0);
  uint8_t *out=malloc(len?len:1);if(!err&&ce_cpk_read(&c,off,out,len)!=(int)len)err="range read failed";
  if(!err)fwrite(out,1,len,stdout);ce_cpk_close(&c);fclose(f);free(d);free(out);if(err){fprintf(stderr,"%s\n",err);return 1;}return 0;}
 if((argc==4||argc==5)&&!strcmp(argv[1],"icon")){
  /* icon <exe|cpk> <out.gzi> [dict]: the program's icon, as the runner caches it. */
  FILE *f=fopen(argv[2],"rb");if(!f)return 2;fseek(f,0,SEEK_END);uint32_t n=(uint32_t)ftell(f),dn=0;void *d=argc==5?file(argv[4],&dn):NULL;
  uint8_t *gzi=NULL;uint32_t size=0;const char *err;
  if(d){struct ce_cpk_file c;err=ce_cpk_open(&c,rd,f,n,d,dn);if(!err)err=ce_exe_icon(ce_cpk_read,&c,c.size,CE_ICON_SIZE,&gzi,&size);ce_cpk_close(&c);}
  else err=ce_exe_icon(rd,f,n,CE_ICON_SIZE,&gzi,&size);
  fclose(f);free(d);if(err){fprintf(stderr,"%s\n",err);free(gzi);return 1;}
  FILE *o=fopen(argv[3],"wb");fwrite(gzi,1,size,o);fclose(o);free(gzi);return 0;}
 return 2;
}
