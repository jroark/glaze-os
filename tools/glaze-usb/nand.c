/* Physical capture: no NAND writes and no ECC correction. Backups require
 * a matching second read; optional source snapshots use one pass. Both are
 * published privately without overwriting an existing file. */
#include "client.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int nand_capture(struct gu_client *c, const char *path, int verify_source)
{
    if (!(c->caps & GU_CAP_NAND_BACKUP)) {
        fputs("Reload a build with physical NAND backup support first.\n", stderr);
        return -1;
    }
    size_t size = strlen(path)+9;
    char *partial = malloc(size);
    if (!partial) return -1;
    snprintf(partial,size,"%s.partial",path);
    if (!access(path,F_OK)) { free(partial); return -1; }
    int fd = open(partial,O_RDWR|O_CREAT|O_EXCL,0600);
    if (fd < 0) { free(partial); return -1; }
    FILE *f = fdopen(fd,"w+b");
    if (!f) { close(fd); free(partial); return -1; }
    int result = -1;
    uint8_t request[8], verify[7*528];
    for (unsigned pass=0;pass<(verify_source ? 2u : 1u);pass++) {
        if (fflush(f) || fseek(f,0,SEEK_SET)) goto done;
        unsigned progress=0;
        for (uint32_t page=0;page<32768;) {
            uint32_t count=32768-page;
            if (count>7) count=7;
            gu_put32(request,page); gu_put32(request+4,count);
            int n=gu_call(c,GU_NAND_READ,request,sizeof(request));
            if (n != (int)(count*528)) goto done;
            if (!pass) {
                if (fwrite(c->frame+GU_HEADER,1,(size_t)n,f) != (size_t)n) goto done;
            } else {
                if (fread(verify,1,(size_t)n,f) != (size_t)n || memcmp(verify,c->frame+GU_HEADER,(size_t)n)) {
                    fprintf(stderr,"NAND changed between reads at physical page %u; backup rejected\n",page);
                    goto done;
                }
            }
            page+=count;
            if (page/2048 != progress) {
                progress=page/2048;
                if (verify_source) fprintf(stderr,"NAND backup pass %u: %u/32768 pages\n",pass+1,page);
                else fprintf(stderr,"NAND scan: %u/32768 pages\n",page);
            }
        }
    }
    if (fflush(f) || fsync(fd)) goto done;
    /* link() publishes without overwriting a file that appeared meanwhile. */
    if (link(partial,path)) goto done;
    if (unlink(partial)) goto done;
    fprintf(stderr,"%s 17301504-byte physical NAND snapshot: %s\n",verify_source ? "Verified" : "Read",path);
    result=0;
done:
    fclose(f);
    if (result) fprintf(stderr,"Backup incomplete; private partial file: %s\n",partial);
    free(partial);
    return result;
}

int gu_nand_backup(struct gu_client *c, const char *path)
{
    return nand_capture(c, path, 1);
}
int gu_nand_snapshot(struct gu_client *c, const char *path)
{
    return nand_capture(c, path, 0);
}
