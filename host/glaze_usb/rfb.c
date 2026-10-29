/* Small RFB 3.3/3.7/3.8 server for a localhost viewer. Raw true-colour
 * rectangles are mandatory RFB functionality; no viewer plug-in is needed.
 * Device authentication/encryption remains on the upstream USB/TLS link. */
#include "rfb.h"
#include <glaze/abi.h>
#include <glaze/mirror.h>
#include <glaze/usb_wire.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
extern volatile sig_atomic_t usb_cancelled;

static uint64_t now(void)
{
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+(unsigned)t.tv_nsec/1000000;
}
static unsigned get16(const uint8_t *p){return (unsigned)p[0]*256+p[1];}
static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void put16(uint8_t *p,unsigned n){p[0]=n>>8;p[1]=n;}
static void put32(uint8_t *p,uint32_t n){p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n;}
static int transfer(int fd,void *buffer,unsigned bytes,int writing)
{
    uint8_t *p=buffer;uint64_t end=now()+5000;
    while(bytes && !usb_cancelled && now()<end){
        ssize_t n=writing?send(fd,p,bytes,0):recv(fd,p,bytes,0);
        if(n>0){p+=n;bytes-=(unsigned)n;continue;}
        if(!n)return -1;
        if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)return -1;
        struct pollfd wait={.fd=fd,.events=writing?POLLOUT:POLLIN};poll(&wait,1,50);
    }
    return bytes?-1:0;
}
struct pixel_format { unsigned bytes,big;uint32_t max[3],shift[3]; };
static int pixel_format(struct pixel_format *f,const uint8_t *p)
{
    if((p[0]!=8 && p[0]!=16 && p[0]!=32) || !p[1] || p[1]>p[0] || p[2]>1 || p[3]!=1)return -1;
    struct pixel_format next={.bytes=p[0]/8,.big=p[2]};uint64_t used=0;unsigned bits=0;
    for(unsigned i=0;i<3;i++){
        unsigned max=get16(p+4+i*2),shift=p[10+i];
        if(!max || (max&(max+1)) || shift>=p[0])return -1;
        uint64_t mask=(uint64_t)max<<shift;
        if(mask>>p[0] || (used&mask))return -1;
        used|=mask;for(unsigned n=max;n;n>>=1)bits++;
        next.max[i]=max;next.shift[i]=shift;
    }
    if(bits>p[1])return -1;
    *f=next;return 0;
}
static int handshake(int fd,struct pixel_format *format)
{
    uint8_t version[12];memcpy(version,"RFB 003.008\n",12);
    if(transfer(fd,version,12,1) || transfer(fd,version,12,0))return -1;
    unsigned minor;
    if(!memcmp(version,"RFB 003.003\n",12))minor=3;
    else if(!memcmp(version,"RFB 003.007\n",12))minor=7;
    else if(!memcmp(version,"RFB 003.008\n",12))minor=8;
    else return -1;
    uint8_t security[4]={0,0,0,1},selected;
    if(minor==3){if(transfer(fd,security,4,1))return -1;}
    else{
        security[0]=1;security[1]=1;
        if(transfer(fd,security,2,1) || transfer(fd,&selected,1,0) || selected!=1)return -1;
        if(minor==8){memset(security,0,4);if(transfer(fd,security,4,1))return -1;}
    }
    if(transfer(fd,&selected,1,0))return -1; /* ClientInit shared flag */
    const char name[]="Glaze BE-300";uint8_t init[24+sizeof(name)-1]={0};
    put16(init,GM_WIDTH);put16(init+2,GM_HEIGHT);
    uint8_t fmt[16]={16,16,0,1,0,31,0,63,0,31,11,5,0,0,0,0};
    memcpy(init+4,fmt,16);put32(init+20,sizeof(name)-1);memcpy(init+24,name,sizeof(name)-1);
    if(pixel_format(format,fmt) || transfer(fd,init,sizeof(init),1))return -1;
    return 0;
}
struct viewer {
    struct pixel_format format;
    uint8_t sent[GU_SCREEN_BYTES],valid[GM_WIDTH*GM_HEIGHT/8];
    uint8_t input[4096];unsigned used,skip;
    unsigned x,y,w,h,pending,incremental;
    unsigned buttons,mouse_x,mouse_y;
    unsigned modifiers;
    uint8_t held[384]; /* ASCII and X11 0xffxx symbols: release translated code */
};
static unsigned special_key(uint32_t symbol)
{
    switch(symbol){
    case 8:case 0xff08:return GZ_KEY_BACKSPACE;
    case 9:case 0xff09:return GZ_KEY_TAB;
    case 13:case 0xff0d:case 0xff8d:return GZ_KEY_ENTER;
    case 27:case 0xff1b:return GZ_KEY_ESC;
    case 127:case 0xffff:return GZ_KEY_DELETE;
    case 0xff50:return GZ_KEY_KBD_HOME;
    case 0xff51:return GZ_KEY_LEFT;
    case 0xff52:return GZ_KEY_UP;
    case 0xff53:return GZ_KEY_RIGHT;
    case 0xff54:return GZ_KEY_DOWN;
    case 0xff55:return GZ_KEY_PAGEUP;
    case 0xff56:return GZ_KEY_PAGEDOWN;
    case 0xff57:return GZ_KEY_END;
    case 0xffbe:return GZ_KEY_HOME;
    case 0xffbf:return GZ_KEY_POWER;
    case 0xffc0:return GZ_KEY_OK;
    default:return 0;
    }
}
static void event(uint8_t *p,unsigned type,unsigned code,unsigned x,unsigned y)
{
    p[0]=type;p[1]=type>>8;p[2]=code;p[3]=code>>8;
    p[4]=x;p[5]=x>>8;p[6]=y;p[7]=y>>8;
}
static int pointer(struct viewer *v,const struct rfb_source *source,const uint8_t *p)
{
    unsigned buttons=p[1],x=get16(p+2),y=get16(p+4),bytes=0;uint8_t events[64];
    if(x>=GM_WIDTH)x=GM_WIDTH-1;
    if(y>=GM_HEIGHT)y=GM_HEIGHT-1;
    unsigned before=v->buttons;
    if((buttons^before)&1){event(events+bytes,buttons&1?GZ_EV_TOUCH_DOWN:GZ_EV_TOUCH_UP,0,x,y);bytes+=8;}
    else if((buttons&1) && (x!=v->mouse_x || y!=v->mouse_y)){event(events+bytes,GZ_EV_TOUCH_MOVE,0,x,y);bytes+=8;}
    for(unsigned bit=1;bit<3;bit++)if((buttons^before)&(1u<<bit)){
        event(events+bytes,buttons&(1u<<bit)?GZ_EV_KEY_DOWN:GZ_EV_KEY_UP,bit==1?GZ_KEY_HOME:GZ_KEY_ESC,0,0);bytes+=8;
    }
    for(unsigned bit=3;bit<5;bit++)if((buttons&~before)&(1u<<bit)){
        unsigned code=bit==3?GZ_KEY_UP:GZ_KEY_DOWN;
        event(events+bytes,GZ_EV_KEY_DOWN,code,0,0);bytes+=8;
        event(events+bytes,GZ_EV_KEY_UP,code,0,0);bytes+=8;
    }
    v->buttons=buttons;v->mouse_x=x;v->mouse_y=y;
    return bytes && source->input(source->context,events,bytes)?-2:0;
}
/* Consume a complete viewer message, or wait for more bytes. */
static int message(struct viewer *v,const struct rfb_source *source,int view_only)
{
    if(!v->used)return 0;
    uint8_t *p=v->input;unsigned need=0;
    switch(p[0]){
    case 0:need=20;break;                       /* SetPixelFormat */
    case 2:if(v->used<4)return 0;             /* SetEncodings */
        if(get16(p+2)>256)return -1;
        need=4+get16(p+2)*4;break;
    case 3:need=10;break;                       /* FramebufferUpdateRequest */
    case 4:need=8;break;                        /* KeyEvent */
    case 5:need=6;break;                        /* PointerEvent */
    case 6:need=8;break;                        /* ClientCutText, discarded */
    default:return -1;
    }
    if(v->used<need)return 0;
    if(p[0]==0){if(pixel_format(&v->format,p+4))return -1;memset(v->valid,0,sizeof(v->valid));}
    if(p[0]==3){
        unsigned x=get16(p+2),y=get16(p+4),w=get16(p+6),h=get16(p+8);
        if(x>GM_WIDTH)x=GM_WIDTH;
        if(y>GM_HEIGHT)y=GM_HEIGHT;
        if(w>GM_WIDTH-x)w=GM_WIDTH-x;
        if(h>GM_HEIGHT-y)h=GM_HEIGHT-y;
        if(v->pending){
            unsigned right=v->x+v->w>x+w?v->x+v->w:x+w,bottom=v->y+v->h>y+h?v->y+v->h:y+h;
            if(x<v->x)v->x=x;
            if(y<v->y)v->y=y;
            v->w=right-v->x;v->h=bottom-v->y;
            v->incremental=v->incremental && p[1];
        }else{v->x=x;v->y=y;v->w=w;v->h=h;v->incremental=p[1]!=0;}
        v->pending=1;
    }
    if(p[0]==4 && !view_only){
        uint32_t symbol=get32(p+4);if((symbol>>24)==1)symbol&=0xffffff;
        unsigned modifier=symbol==0xffe1?1:symbol==0xffe2?2:symbol==0xffe3?4:
            symbol==0xffe4?8:symbol==0xffe7?16:symbol==0xffe8?32:
            symbol==0xffeb?64:symbol==0xffec?128:0;
        if(modifier){if(p[1])v->modifiers|=modifier;else v->modifiers&=~modifier;goto consumed;}
        unsigned code=special_key(symbol);uint8_t e[8];
        unsigned slot=symbol<128?symbol:(symbol>=0xff00 && symbol<=0xffff)?128+(symbol&255):384;
        if(!p[1] && slot<384 && v->held[slot]){
            event(e,GZ_EV_KEY_UP,v->held[slot],0,0);v->held[slot]=0;
            if(source->input(source->context,e,8))return -2;
            goto consumed;
        }
        if(v->modifiers&0xfc){
            unsigned ch=symbol>='A' && symbol<='Z'?symbol+32:symbol;
            code=ch=='a'?GZ_KEY_SELECT_ALL:ch=='c'?GZ_KEY_COPY:ch=='x'?GZ_KEY_CUT:ch=='v'?GZ_KEY_PASTE:0;
            if(!code)goto consumed;
        }else if(v->modifiers&3){
            if(code==GZ_KEY_LEFT)code=GZ_KEY_SELECT_LEFT;
            if(code==GZ_KEY_RIGHT)code=GZ_KEY_SELECT_RIGHT;
            if(code==GZ_KEY_KBD_HOME)code=GZ_KEY_SELECT_HOME;
            if(code==GZ_KEY_END)code=GZ_KEY_SELECT_END;
        }
        if(code){event(e,p[1]?GZ_EV_KEY_DOWN:GZ_EV_KEY_UP,code,0,0);if(p[1] && slot<384)v->held[slot]=code;}
        else if(p[1] && symbol>=32 && symbol<=126)event(e,GZ_EV_TEXT,symbol,0,0);
        else goto consumed;
        if(source->input(source->context,e,8))return -2;
    }
    if(p[0]==5 && !view_only){int rv=pointer(v,source,p);if(rv)return rv;}
    if(p[0]==6){if(get32(p+4)>1024*1024)return -1;v->skip=get32(p+4);}
consumed:
    v->used-=need;memmove(v->input,v->input+need,v->used);return 1;
}
static int update(int fd,struct viewer *v,const uint8_t *pixels)
{
    unsigned x=v->x,y=v->y,w=v->w,h=v->h;
    int changed=!v->incremental;
    for(unsigned row=y;!changed && row<y+h;row++)for(unsigned col=x;col<x+w;col++){
        unsigned i=row*GM_WIDTH+col;
        if(!(v->valid[i/8]&(1u<<(i%8))) || memcmp(v->sent+i*2,pixels+i*2,2)){changed=1;break;}
    }
    if(w && h && !changed)return 0; /* Keep incremental request pending. */
    uint8_t header[16]={0};put16(header+2,(w && h)?1:0);
    if(w && h){put16(header+4,x);put16(header+6,y);put16(header+8,w);put16(header+10,h);}
    if(transfer(fd,header,w && h?16:4,1))return -1;
    for(unsigned row=y;w && row<y+h;row++){
        uint8_t line[GM_WIDTH*4];unsigned pos=0;
        for(unsigned col=x;col<x+w;col++){
            unsigned i=(row*GM_WIDTH+col)*2,value=pixels[i]|(unsigned)pixels[i+1]<<8;
            uint32_t rgb=((value>>11)*v->format.max[0]+15)/31<<v->format.shift[0] |
                (((value>>5&63)*v->format.max[1]+31)/63)<<v->format.shift[1] |
                (((value&31)*v->format.max[2]+15)/31)<<v->format.shift[2];
            for(unsigned b=0;b<v->format.bytes;b++)line[pos++]=(uint8_t)(rgb>>((v->format.big?v->format.bytes-1-b:b)*8));
        }
        if(transfer(fd,line,pos,1))return -1;
    }
    for(unsigned row=y;row<y+h;row++)for(unsigned col=x;col<x+w;col++){
        unsigned i=row*GM_WIDTH+col;memcpy(v->sent+i*2,pixels+i*2,2);v->valid[i/8]|=1u<<(i%8);
    }
    v->pending=0;return 0;
}
int rfb_session(int fd,const struct rfb_source *source,unsigned fps,int view_only)
{
    if(!fps || fps>15)return -1;
    struct viewer *v=calloc(1,sizeof(*v));if(!v)return -1;
    int rv=-1;
    if(handshake(fd,&v->format))goto done;
    const uint8_t *pixels=NULL;
    if(source->capture(source->context,&pixels)){rv=-2;goto done;}
    uint64_t next=now(),last=now();
    while(!usb_cancelled){
        /* Large clipboard messages are streamed away without allocations. */
        while(v->skip && v->used){unsigned n=v->skip<v->used?v->skip:v->used;v->skip-=n;v->used-=n;memmove(v->input,v->input+n,v->used);}
        for(unsigned budget=0;!v->skip && budget<64;budget++){
            int r=message(v,source,view_only);if(r<0){rv=r;goto done;}if(!r)break;
            if(v->skip)break;
        }
        uint64_t t=now();
        if(v->pending && t>=next){
            if(source->capture(source->context,&pixels)){rv=-2;goto done;}
            last=now();next=last+1000/fps;
            if(update(fd,v,pixels))goto done;
        }else if(t-last>=1000 && source->keepalive){
            if(source->keepalive(source->context)){rv=-2;goto done;}last=now();
        }
        struct pollfd wait={.fd=fd,.events=POLLIN};int ready=poll(&wait,1,10);
        if(ready<0){if(errno==EINTR)continue;goto done;}
        if(ready>0 && (wait.revents&POLLIN)){
            if(v->used==sizeof(v->input))goto done;
            ssize_t n=recv(fd,v->input+v->used,sizeof(v->input)-v->used,0);
            if(!n){rv=0;goto done;}
            if(n<0){if(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)continue;goto done;}
            v->used+=(unsigned)n;
        }else if(ready>0 && (wait.revents&(POLLERR|POLLHUP|POLLNVAL))){rv=0;goto done;}
    }
    rv=0;
done:
    free(v);return rv;
}
