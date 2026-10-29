/* Atomic snapshots with bounded 16x16 deltas; no app surface restrictions. */
#include "mirror.h"
#include <glaze/usb.h>
#include <glaze/mirror.h>
#include <stdlib.h>
#include <string.h>
#if !defined(__mips__)
extern int gz_usb_ctl(uint32_t,void *,uint32_t);
#endif
static uint8_t *pixels;
static uint32_t hashes[GM_TILES];
static uint16_t changed[GM_TILES];
static unsigned count,read_until;
static bool baseline;
int mirror_active(void){return pixels!=NULL;}
void mirror_close(void)
{
    free(pixels);pixels=NULL;baseline=false;count=read_until=0;
    memset(hashes,0,sizeof(hashes));
    gz_usb_ctl(GZ_USB_SCRIPT_CANCEL,NULL,0);
}
static uint32_t tile_hash(unsigned tile)
{
    uint32_t hash=2166136261u;
    unsigned start=((tile/GM_COLUMNS)*GM_TILE*GM_WIDTH+(tile%GM_COLUMNS)*GM_TILE)*2;
    for(unsigned y=0;y<GM_TILE;y++)for(unsigned x=0;x<GM_TILE*2;x++)
        hash=(hash^pixels[start+y*GM_WIDTH*2+x])*16777619u;
    return hash;
}
int mirror_dispatch(uint32_t op,const uint8_t *in,uint32_t n,uint8_t *out,uint32_t *size)
{
    *size=0;
    if(op==GU_MIRROR_CLOSE){if(n)return GZ_EINVAL;mirror_close();return 0;}
    if(op==GU_MIRROR_INPUT){
        if(!pixels || !n || n%8 || n>GM_INPUT_BATCH*8)return GZ_EINVAL;
        return gz_usb_ctl(GZ_USB_LIVE_INPUT,(void *)in,n);
    }
    if(op==GU_MIRROR_NEXT){
        if(n!=4 || (gu_get32(in)&~GM_FORCE))return GZ_EINVAL;
        bool force=gu_get32(in)&GM_FORCE;
        if((!baseline || read_until<count*GM_RECORD) && !force)return GZ_EINVAL;
        if(!pixels){
            pixels=malloc(GU_SCREEN_BYTES);if(!pixels)return GZ_ENOMEM;
            gz_usb_ctl(GZ_USB_SCRIPT_CANCEL,NULL,0);
        }
        int rv=gz_usb_ctl(GZ_USB_SCREEN_COPY,pixels,GU_SCREEN_BYTES);
        if(rv){mirror_close();return rv;}
        count=0;
        for(unsigned i=0;i<GM_TILES;i++){
            uint32_t h=tile_hash(i);
            if(force || h!=hashes[i])changed[count++]=(uint16_t)i;
            hashes[i]=h;
        }
        baseline=true;read_until=0;
        gu_put32(out,GM_WIDTH);gu_put32(out+4,GM_HEIGHT);gu_put32(out+8,GM_TILE);
        gu_put32(out+12,count);gu_put32(out+16,count*GM_RECORD);
        gu_put32(out+20,~gu_crc(~0u,pixels,GU_SCREEN_BYTES));*size=24;return 0;
    }
    if(op!=GU_MIRROR_READ || !pixels || n!=4)return GZ_EINVAL;
    unsigned off=gu_get32(in),total=count*GM_RECORD;
    if(off>=total || off>read_until)return GZ_EINVAL;
    unsigned bytes=total-off>GU_PAYLOAD?GU_PAYLOAD:total-off;
    for(unsigned i=0;i<bytes;i++){
        unsigned pos=off+i,tile=changed[pos/GM_RECORD],within=pos%GM_RECORD;
        if(within<4)out[i]=(uint8_t)(tile>>(within*8));
        else{
            within-=4;
            unsigned x=(tile%GM_COLUMNS)*GM_TILE*2+within%(GM_TILE*2);
            unsigned y=(tile/GM_COLUMNS)*GM_TILE+within/(GM_TILE*2);
            out[i]=pixels[y*GM_WIDTH*2+x];
        }
    }
    if(off+bytes>read_until)read_until=off+bytes;
    *size=bytes;return 0;
}
