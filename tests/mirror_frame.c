#include <glaze/usb.h>
#include <glaze/mirror.h>
#include "../servers/usb/mirror.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
static uint8_t screen[GU_SCREEN_BYTES],stream[GM_TILES*GM_RECORD];
static int copy_error;
static unsigned releases;
int gz_usb_ctl(uint32_t op,void *buf,uint32_t len)
{
    if(op==GZ_USB_SCRIPT_CANCEL){releases++;return 0;}
    if(op==GZ_USB_LIVE_INPUT)return 123;
    assert(op==GZ_USB_SCREEN_COPY && len==sizeof(screen));
    if(copy_error)return GZ_EIO;
    memcpy(buf,screen,len);return 0;
}
static uint8_t result[GU_PAYLOAD];
static uint32_t size;
static int call(unsigned op,unsigned value,unsigned bytes)
{uint8_t in[GM_INPUT_BATCH*8]={0};gu_put32(in,value);return mirror_dispatch(op,in,bytes,result,&size);}
static void read_all(unsigned total)
{
    for(unsigned off=0;off<total;){
        assert(!call(GU_MIRROR_READ,off,4));assert(size && size<=GU_PAYLOAD && size<=total-off);
        memcpy(stream+off,result,size);off+=size;
    }
}
int main(void)
{
    assert(!mirror_active());assert(call(GU_MIRROR_NEXT,0,4)==GZ_EINVAL);
    assert(call(GU_MIRROR_INPUT,0,4)==GZ_EINVAL);
    for(unsigned i=0;i<sizeof(screen);i++)screen[i]=(uint8_t)(i*17+i/97);
    assert(!call(GU_MIRROR_NEXT,GM_FORCE,4) && size==24 && mirror_active());
    assert(gu_get32(result)==240 && gu_get32(result+4)==320 && gu_get32(result+12)==GM_TILES);
    unsigned total=gu_get32(result+16);assert(total==sizeof(stream));
    assert(gu_get32(result+20)==~gu_crc(~0u,screen,sizeof(screen)));
    assert(call(GU_MIRROR_NEXT,0,4)==GZ_EINVAL);
    assert(call(GU_MIRROR_READ,total,4)==GZ_EINVAL);
    assert(call(GU_MIRROR_READ,1,4)==GZ_EINVAL);
    assert(!call(GU_MIRROR_READ,0,4));uint8_t first[GU_PAYLOAD];memcpy(first,result,size);
    assert(!call(GU_MIRROR_READ,0,4) && !memcmp(first,result,size));
    read_all(total);
    for(unsigned i=0;i<GM_TILES;i++){
        uint8_t *p=stream+i*GM_RECORD;assert(gu_get32(p)==i);
        unsigned base=((i/GM_COLUMNS)*GM_TILE*GM_WIDTH+(i%GM_COLUMNS)*GM_TILE)*2;
        for(unsigned row=0;row<GM_TILE;row++)assert(!memcmp(p+4+row*GM_TILE*2,screen+base+row*GM_WIDTH*2,GM_TILE*2));
    }
    assert(!call(GU_MIRROR_NEXT,0,4) && !gu_get32(result+12) && !gu_get32(result+16));
    screen[sizeof(screen)-1]^=0x80;
    assert(!call(GU_MIRROR_NEXT,0,4) && gu_get32(result+12)==1);
    read_all(GM_RECORD);assert(gu_get32(stream)==GM_TILES-1 && stream[GM_RECORD-1]==screen[sizeof(screen)-1]);
    assert(call(GU_MIRROR_INPUT,0,4)==GZ_EINVAL);
    assert(call(GU_MIRROR_INPUT,0,8)==123); /* forwarded through trusted input control */
    assert(!call(GU_MIRROR_NEXT,GM_FORCE,4));assert(!call(GU_MIRROR_NEXT,GM_FORCE,4));
    assert(call(GU_MIRROR_NEXT,2,4)==GZ_EINVAL);
    assert(call(GU_MIRROR_CLOSE,0,4)==GZ_EINVAL && mirror_active());
    assert(!call(GU_MIRROR_CLOSE,0,0) && !mirror_active());
    assert(call(GU_MIRROR_READ,0,4)==GZ_EINVAL);
    copy_error=1;assert(call(GU_MIRROR_NEXT,GM_FORCE,4)==GZ_EIO && !mirror_active());
    assert(releases>=3);
    puts("mirror-frame: atomic snapshot, deltas, boundaries, replay, full resync and cleanup PASS");return 0;
}
