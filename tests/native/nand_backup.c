/* Real backup client against a deterministic physical-page RPC peer. */
#include "../../tools/glaze-usb/client.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#endif
static unsigned calls;
static int mismatch, fail, recoveries, transient, permanent, device_error, publish_race;
static const char *publish_path;
static int recover(struct gu_client *c) { (void)c; recoveries++; fail=0; return 0; }
int gu_call(struct gu_client *c,uint32_t op,const void *req,uint32_t n)
{
    assert(op==GU_NAND_READ && n==8);
    unsigned page=gu_get32(req), count=gu_get32((const uint8_t *)req+4);
    assert(count && count<=7 && page+count<=32768);
    c->transport_failed=0;
    if (device_error) return -1;
    if (fail || (transient && calls==100) || permanent) {
        transient=0; c->transport_failed=1; return -1;
    }
    for(unsigned i=0;i<count*528;i++) c->frame[GU_HEADER+i]=(uint8_t)(page+i/528+i%528);
    calls++;
    if (publish_race && calls==9364) {
        FILE *f=fopen(publish_path,"wb"); assert(f);
        assert(fwrite("keep",1,4,f)==4 && !fclose(f));
    }
    if(mismatch && calls>4682) c->frame[GU_HEADER]^=1;
    return (int)(count*528);
}
int main(void)
{
#ifdef _WIN32
    installer_binary();
    char dir[512], root[256], path[600], partial[620];
    assert(GetTempPathA(sizeof(root),root));
    snprintf(dir,sizeof(dir),"%sglaze-backup-%lu-%llu",root,(unsigned long)GetCurrentProcessId(),(unsigned long long)GetTickCount64());
    assert(!_mkdir(dir)); snprintf(path,sizeof(path),"%s/nand.raw",dir);
#else
    char dir[]="/tmp/glaze-backup-test-XXXXXX", path[256], partial[280];
    assert(mkdtemp(dir)); snprintf(path,sizeof(path),"%s/nand.raw",dir);
#endif
    snprintf(partial,sizeof(partial),"%s.partial",path);
    struct gu_client c={.caps=GU_CAP_NAND_BACKUP};
    assert(!gu_nand_backup(&c,path) && calls==9364);
    struct stat st; assert(!stat(path,&st) && st.st_size==17301504);
#ifndef _WIN32
    assert((st.st_mode&0777)==0600);
#endif
    assert(access(partial,F_OK));
    unsigned previous=calls; assert(gu_nand_backup(&c,path)<0 && calls==previous);
    assert(!unlink(path)); calls=0; mismatch=1;
    assert(gu_nand_backup(&c,path)<0 && access(path,F_OK) && !access(partial,F_OK));
    assert(!unlink(partial)); fail=1;
    assert(gu_nand_backup(&c,path)<0 && access(path,F_OK));
    assert(!unlink(partial));
    calls=0;fail=0;mismatch=0;transient=1;c.recover_read=recover;
    assert(!gu_nand_backup(&c,path) && calls==9364 && recoveries==1);
    assert(!unlink(path));
    calls=0;recoveries=0;permanent=1;
    assert(gu_nand_snapshot(&c,path)<0 && recoveries==3 && !calls && access(path,F_OK));
    assert(!unlink(partial));permanent=0;c.recover_read=NULL;
    calls=0;recoveries=0;device_error=1;c.recover_read=recover;
    assert(gu_nand_snapshot(&c,path)<0 && !recoveries && !calls);
    assert(!unlink(partial));device_error=0;
    publish_race=1;publish_path=path;
    assert(gu_nand_backup(&c,path)<0 && !access(partial,F_OK));
    FILE *existing=fopen(path,"rb");char kept[4];assert(existing);
    assert(fread(kept,1,4,existing)==4 && !memcmp(kept,"keep",4) && !fclose(existing));
    assert(!unlink(partial) && !unlink(path));publish_race=0;c.recover_read=NULL;
    calls=0;fail=0;mismatch=1;
    assert(!gu_nand_snapshot(&c,path) && calls==4682);
    assert(!stat(path,&st) && st.st_size==17301504 && access(partial,F_OK));
    assert(!unlink(path));calls=0;fail=1;
    assert(gu_nand_snapshot(&c,path)<0 && access(path,F_OK) && !access(partial,F_OK));
    assert(!unlink(partial));
    assert(!rmdir(dir));
    puts("nand-backup: full two-pass read, private publication, no overwrite, mismatch and RPC failure PASS");
}
