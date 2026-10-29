/* Production VNC bridge and device delta engine with deterministic pixels.
 * Only the physical/TLS RPC and kernel framebuffer/input boundary are faked. */
#include "../host/glaze_usb/client.h"
#include "../servers/usb/mirror.h"
#include <glaze/usb.h>
#include <glaze/mirror.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
volatile sig_atomic_t usb_cancelled;
static uint8_t framebuffer[GU_SCREEN_BYTES],events[4096];
static unsigned captures,full,stream_bytes,event_bytes,releases;
static int corrupt,touch,keys;
static const char *metrics;
static void record(void)
{
    FILE *f=fopen(metrics,"w");if(!f)abort();
    fprintf(f,"%u %u %u %u %u %d %d\n",captures,full,stream_bytes,event_bytes,releases,touch,keys);
    for(unsigned i=0;i<event_bytes;i++)fprintf(f,"%02x",events[i]);
    fputc('\n',f);fclose(f);
}
int gz_usb_ctl(uint32_t op,void *buffer,uint32_t n)
{
    if(op==GZ_USB_SCREEN_COPY){memcpy(buffer,framebuffer,n);return 0;}
    if(op==GZ_USB_SCRIPT_CANCEL){releases++;touch=keys=0;return 0;}
    if(op==GZ_USB_LIVE_INPUT){
        const uint8_t *p=buffer;if(event_bytes+n>sizeof(events))return GZ_EFULL;
        memcpy(events+event_bytes,p,n);event_bytes+=n;
        for(unsigned i=0;i<n;i+=8){
            unsigned type=p[i],code=p[i+2],x=p[i+4]|(unsigned)p[i+5]<<8,y=p[i+6]|(unsigned)p[i+7]<<8;
            if(type==GZ_EV_TOUCH_DOWN || type==GZ_EV_TOUCH_MOVE){
                unsigned at=(y*GM_WIDTH+x)*2;if(at+1<sizeof(framebuffer))framebuffer[at]=framebuffer[at+1]=255;
            }
            if(type==GZ_EV_TOUCH_DOWN)touch=1;
            if(type==GZ_EV_TOUCH_UP)touch=0;
            if(type==GZ_EV_KEY_DOWN)keys|=1u<<code;
            if(type==GZ_EV_KEY_UP)keys&=~(1u<<code);
        }
        return 0;
    }
    return GZ_EINVAL;
}
int gu_call(struct gu_client *c,uint32_t op,const void *data,uint32_t n)
{
    c->id++;uint32_t size=0;int r;
    if(op==GU_DEBUG_STATUS){memset(c->frame+GU_HEADER,0,8);return 8;}
    r=mirror_dispatch(op,data,n,c->frame+GU_HEADER,&size);
    if(!r && op==GU_MIRROR_NEXT){captures++;if(gu_get32(data)&GM_FORCE)full++;}
    if(!r && op==GU_MIRROR_READ){stream_bytes+=size;if(corrupt && size>8){c->frame[GU_HEADER+8]^=1;corrupt=0;}}
    record();return r?-1:(int)size;
}
static void stop(int signal){(void)signal;usb_cancelled=1;}
int main(int argc,char **argv)
{
    if(argc<3)return 2;
    metrics=argv[1];corrupt=getenv("MIRROR_CORRUPT_ONCE")!=NULL;
    framebuffer[0]=0;framebuffer[1]=0xf8;framebuffer[2]=0xe0;framebuffer[3]=7;framebuffer[4]=31;
    signal(SIGINT,stop);signal(SIGTERM,stop);signal(SIGPIPE,SIG_IGN);
    struct usb_link link={0};struct gu_client c={.usb=&link,.caps=GU_CAP_MIRROR};
    int r=gu_mirror(&c,argc-2,argv+2);mirror_close();record();return r?1:0;
}
