/* Real backup client against a deterministic physical-page RPC peer. */
#include "../host/glaze_usb/client.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static unsigned calls;
static int mismatch, fail;
int gu_call(struct gu_client *c,uint32_t op,const void *req,uint32_t n)
{
    assert(op==GU_NAND_READ && n==8);
    unsigned page=gu_get32(req), count=gu_get32((const uint8_t *)req+4);
    assert(count && count<=7 && page+count<=32768);
    if (fail) return -1;
    for(unsigned i=0;i<count*528;i++) c->frame[GU_HEADER+i]=(uint8_t)(page+i/528+i%528);
    calls++;
    if(mismatch && calls>4682) c->frame[GU_HEADER]^=1;
    return (int)(count*528);
}
int main(void)
{
    char dir[]="/tmp/glaze-backup-test-XXXXXX", path[256], partial[280];
    assert(mkdtemp(dir)); snprintf(path,sizeof(path),"%s/nand.raw",dir);
    snprintf(partial,sizeof(partial),"%s.partial",path);
    struct gu_client c={.caps=GU_CAP_NAND_BACKUP};
    assert(!gu_nand_backup(&c,path) && calls==9364);
    struct stat st; assert(!stat(path,&st) && st.st_size==17301504 && (st.st_mode&0777)==0600);
    assert(access(partial,F_OK));
    unsigned previous=calls; assert(gu_nand_backup(&c,path)<0 && calls==previous);
    assert(!unlink(path)); calls=0; mismatch=1;
    assert(gu_nand_backup(&c,path)<0 && access(path,F_OK) && !access(partial,F_OK));
    assert(!unlink(partial)); fail=1;
    assert(gu_nand_backup(&c,path)<0 && access(path,F_OK));
    assert(!unlink(partial)); assert(!rmdir(dir));
    puts("nand-backup: full two-pass read, private publication, no overwrite, mismatch and RPC failure PASS");
}
