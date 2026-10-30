/* Offline CF fixture using the exact production formatter and file writer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "card.h"
static FILE *image;
static struct gz_cf_info card={.present=1,.generation=1,.model="Offline recovery fixture"};
int card_raw_info(struct gz_cf_info *out) { *out=card;return 0; }
int card_raw_read(uint32_t lba,uint32_t count,void *out)
{
    if (!count || count>8 || lba>=card.sectors || count>card.sectors-lba) return GZ_EINVAL;
    return fseeko(image,(off_t)lba*512,SEEK_SET) || fread(out,512,count,image)!=count ? GZ_EIO : 0;
}
int card_raw_write(uint32_t lba,uint32_t count,const void *data)
{
    if (!count || count>8 || lba>=card.sectors || count>card.sectors-lba) return GZ_EINVAL;
    return fseeko(image,(off_t)lba*512,SEEK_SET) || fwrite(data,512,count,image)!=count ? GZ_EIO : 0;
}
void card_log(const char *s) { fprintf(stderr,"%s\n",s); }
DWORD get_fattime(void) { return 46u<<25 | 10u<<21 | 30u<<16; }
/* The formatter is CF-only. A NAND access in this tool is a bug. */
int nand_raw_read(uint32_t p,void *b) { (void)p;(void)b;abort(); }
int nand_raw_prog(uint32_t p,const void *b) { (void)p;(void)b;abort(); }
int nand_raw_erase(uint32_t b) { (void)b;abort(); }
int main(int argc,char **argv)
{
    if (argc!=4) { fputs("usage: mkcf-recovery SECTORS RECOVER.BIN NEW_IMAGE\n",stderr);return 2; }
    char *end;errno=0;unsigned long n=strtoul(argv[1],&end,0);
    if (errno || !*argv[1] || *end || n>UINT32_MAX) return 2;
    card.sectors=(uint32_t)n;
    FILE *payload=fopen(argv[2],"rb");if(!payload)return 1;
    int fd=open(argv[3],O_RDWR|O_CREAT|O_EXCL,0600);if(fd<0){fclose(payload);return 1;}
    image=fdopen(fd,"w+b");if(!image){close(fd);fclose(payload);return 1;}
    if (ftruncate(fd,(off_t)card.sectors*512)) return 1;
    /* A stale geometry/identity must fail before any sector write. */
    if (card_format_recovery(2,card.sectors)!=GZ_ENOENT) abort();
    if (card_format_recovery(1,card.sectors)) return 1;
    struct card_file f;
    if (card_open(&f,"RECOVER.BIN",FS_O_WRITE|FS_O_CREATE|FS_O_EXCL)) return 1;
    uint8_t buf[4096];size_t got;
    while ((got=fread(buf,1,sizeof(buf),payload)))
        if (card_write(&f,buf,(uint32_t)got)!=(int)got) return 1;
    if (ferror(payload) || card_close(&f) || fflush(image) || fsync(fd)) return 1;
    fclose(payload);fclose(image);return 0;
}
