/* Read-only CF extent snapshots, checked twice and bound to one insertion. */
#include "client.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int gu_cf_info(struct gu_client *c, uint32_t *sectors, uint32_t *generation)
{
    if (!(c->caps & GU_CAP_CF_READ)) {
        fputs("Reload a build with CF inspection support first.\n",stderr);
        return -1;
    }
    if (gu_call(c,GU_CF_INFO,NULL,0)!=56) return -1;
    const uint8_t *b=c->frame+GU_HEADER;
    if (!gu_get32(b)) { fputs("No ATA CF card present.\n",stderr); return -1; }
    *generation=gu_get32(b+4); *sectors=gu_get32(b+8);
    if (!*sectors) return -1;
    fprintf(stderr,"CF: %.44s, %u sectors, generation %u\n",b+12,*sectors,*generation);
    return 0;
}

int gu_cf_read(struct gu_client *c, uint32_t lba, uint32_t count, const char *path)
{
    uint32_t sectors,generation;
    if (gu_cf_info(c,&sectors,&generation)) return -1;
    if (!count || lba>=sectors || count>sectors-lba) return -1;
    size_t path_size=strlen(path)+9;
    char *partial=malloc(path_size);
    if (!partial) return -1;
    snprintf(partial,path_size,"%s.partial",path);
    if (!access(path,F_OK)) { free(partial); return -1; }
    int fd=open(partial,O_RDWR|O_CREAT|O_EXCL,0600);
    if (fd<0) { free(partial); return -1; }
    FILE *f=fdopen(fd,"w+b");
    if (!f) { close(fd); free(partial); return -1; }
    int result=-1;
    uint8_t request[12],verify[4096];
    for (unsigned pass=0;pass<2;pass++) {
        if (fflush(f) || fseeko(f,0,SEEK_SET)) goto done;
        for (uint32_t at=0;at<count;) {
            uint32_t n=count-at; if(n>8)n=8;
            gu_put32(request,lba+at); gu_put32(request+4,n); gu_put32(request+8,generation);
            if (gu_call(c,GU_CF_READ,request,sizeof(request))!=(int)(n*512)) goto done;
            if (!pass) {
                if (fwrite(c->frame+GU_HEADER,512,n,f)!=n) goto done;
            } else if (fread(verify,512,n,f)!=n || memcmp(verify,c->frame+GU_HEADER,n*512)) {
                fprintf(stderr,"CF changed between reads at LBA %u; snapshot rejected\n",lba+at);
                goto done;
            }
            at+=n;
        }
    }
    uint32_t final_sectors,final_generation;
    if (gu_cf_info(c,&final_sectors,&final_generation) || final_sectors!=sectors || final_generation!=generation)
        goto done;
    if (fflush(f) || fsync(fd) || link(partial,path) || unlink(partial)) goto done;
    fprintf(stderr,"Verified read-only CF extent: LBA %u + %u, %s\n",lba,count,path);
    result=0;
done:
    fclose(f);
    if (result) fprintf(stderr,"CF snapshot incomplete; private partial file: %s\n",partial);
    free(partial);
    return result;
}
