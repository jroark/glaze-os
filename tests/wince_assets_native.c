#include "../apps/wince/assets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int rd(void *arg,uint32_t off,void *p,uint32_t n){FILE *f=arg;if(fseek(f,off,SEEK_SET))return -1;return (int)fread(p,1,n,f);}
static void *file(const char *p,uint32_t *n){FILE *f=fopen(p,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);*n=(uint32_t)ftell(f);rewind(f);void *b=malloc(*n);if(fread(b,1,*n,f)!=*n)exit(2);fclose(f);return b;}
int main(int argc,char **argv){
 if(argc==5&&!strcmp(argv[1],"recover")){FILE *f=fopen(argv[2],"rb");if(!f)return 2;struct ce_nk_assets a;const char *err=ce_nk_recover(rd,f,NULL,&a);fclose(f);if(err){fprintf(stderr,"%s\n",err);return 1;}FILE *o=fopen(argv[3],"wb");fwrite(a.ordinals,1,a.ordinal_bytes,o);fclose(o);o=fopen(argv[4],"wb");fwrite(a.dictionary,1,a.dictionary_bytes,o);fclose(o);printf("%u ordinals bytes, %u dictionary bytes\n",a.ordinal_bytes,a.dictionary_bytes);ce_nk_assets_free(&a);return 0;}
 if(argc==5&&!strcmp(argv[1],"decode")){uint32_t n,dn;void *b=file(argv[2],&n),*d=file(argv[3],&dn),*out=NULL;uint32_t size;const char *err=ce_cpk_unpack(b,n,d,dn,&out,&size);free(b);free(d);if(err){fprintf(stderr,"%s\n",err);return 1;}FILE *f=fopen(argv[4],"wb");fwrite(out,1,size,f);fclose(f);free(out);return 0;}
 return 2;
}
