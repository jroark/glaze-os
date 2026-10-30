/* Read-only CF snapshot client: no device and no write RPC. */
#include "../host/glaze_usb/client.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static unsigned calls;
static int changed,removed,short_read;
int gu_call(struct gu_client *c,uint32_t op,const void *req,uint32_t size)
{
    uint8_t *b=c->frame+GU_HEADER;
    if (op==GU_CF_INFO) {
        assert(!size);memset(b,0,56);
        gu_put32(b,!removed);gu_put32(b+4,7);gu_put32(b+8,1000);
        memcpy(b+12,"CF test fixture",15);return 56;
    }
    assert(op==GU_CF_READ && size==12);
    uint32_t lba=gu_get32(req),n=gu_get32((const uint8_t *)req+4);
    assert(n && n<=8 && lba+n<=1000 && gu_get32((const uint8_t *)req+8)==7);
    for(unsigned i=0;i<n*512;i++) b[i]=(uint8_t)(lba+i/512+i);
    if (changed && calls>=3)b[0]^=1;
    calls++;return short_read ? (int)(n*512)-1 : (int)(n*512);
}
int main(void)
{
    char dir[]="/tmp/glaze-cf-test-XXXXXX",path[256],partial[280];assert(mkdtemp(dir));
    snprintf(path,sizeof(path),"%s/extent",dir);snprintf(partial,sizeof(partial),"%s.partial",path);
    struct gu_client c={.caps=GU_CAP_CF_READ};
    assert(!gu_cf_read(&c,12,19,path) && calls==6);
    struct stat st;assert(!stat(path,&st) && st.st_size==19*512 && (st.st_mode&0777)==0600);
    assert(gu_cf_read(&c,12,19,path)<0 && calls==6);unlink(path);
    calls=0;changed=1;
    assert(gu_cf_read(&c,12,19,path)<0 && access(path,F_OK) && !access(partial,F_OK));unlink(partial);
    changed=0;short_read=1;
    assert(gu_cf_read(&c,12,19,path)<0 && access(path,F_OK));unlink(partial);
    short_read=0;unsigned old=calls;
    assert(gu_cf_read(&c,999,2,path)<0 && gu_cf_read(&c,0,0,path)<0 && calls==old);
    removed=1;assert(gu_cf_read(&c,12,19,path)<0 && access(partial,F_OK));
    rmdir(dir);puts("cf-read: two-pass exact extent, identity, private publication, bounds, mismatch and short reads PASS");
}
