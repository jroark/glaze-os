/* Local VNC bridge over the physical USB or paired TLS development link. */
#include "client.h"
#include "rfb.h"
#include <glaze/mirror.h>
#include <glaze/abi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>

struct mirror_client {
    struct gu_client *client;
    uint8_t pixels[GU_SCREEN_BYTES],next[GU_SCREEN_BYTES],stream[GM_TILES*GM_RECORD];
    int baseline,failed;
};
static int capture(void *context,const uint8_t **pixels)
{
    struct mirror_client *m=context;
    for(unsigned attempt=0;attempt<2;attempt++){
        uint8_t request[4];gu_put32(request,(!m->baseline || attempt)?GM_FORCE:0);
        int n=gu_call(m->client,GU_MIRROR_NEXT,request,4);
        const uint8_t *h=m->client->frame+GU_HEADER;
        if(n!=24 || gu_get32(h)!=GM_WIDTH || gu_get32(h+4)!=GM_HEIGHT || gu_get32(h+8)!=GM_TILE)break;
        unsigned count=gu_get32(h+12),total=gu_get32(h+16);uint32_t crc=gu_get32(h+20);
        if(count>GM_TILES || total!=count*GM_RECORD || ((!m->baseline || attempt) && count!=GM_TILES))break;
        for(unsigned off=0;off<total;){
            gu_put32(request,off);n=gu_call(m->client,GU_MIRROR_READ,request,4);
            if(n<=0 || (unsigned)n>total-off)goto failed;
            memcpy(m->stream+off,m->client->frame+GU_HEADER,(unsigned)n);off+=(unsigned)n;
        }
        memcpy(m->next,m->pixels,GU_SCREEN_BYTES);
        uint8_t seen[GM_TILES]={0};
        for(unsigned i=0;i<count;i++){
            const uint8_t *record=m->stream+i*GM_RECORD;
            unsigned tile=gu_get32(record);if(tile>=GM_TILES || seen[tile])goto failed;
            seen[tile]=1;
            unsigned start=((tile/GM_COLUMNS)*GM_TILE*GM_WIDTH+(tile%GM_COLUMNS)*GM_TILE)*2;
            for(unsigned row=0;row<GM_TILE;row++)memcpy(m->next+start+row*GM_WIDTH*2,record+4+row*GM_TILE*2,GM_TILE*2);
        }
        if(~gu_crc(~0u,m->next,GU_SCREEN_BYTES)!=crc)continue; /* recover with a full frame */
        memcpy(m->pixels,m->next,GU_SCREEN_BYTES);m->baseline=1;*pixels=m->pixels;return 0;
    }
failed:
    m->failed=1;fputs("Mirroring lost a valid device frame\n",stderr);return -1;
}
static int input(void *context,const uint8_t *events,unsigned bytes)
{
    struct mirror_client *m=context;
    if(gu_call(m->client,GU_MIRROR_INPUT,events,bytes)){m->failed=1;return -1;}return 0;
}
static int keepalive(void *context)
{
    struct mirror_client *m=context;
    if(gu_call(m->client,GU_DEBUG_STATUS,NULL,0)!=8){m->failed=1;return -1;}return 0;
}
static unsigned number(const char *text,unsigned max)
{
    char *end;errno=0;unsigned long n=strtoul(text,&end,10);
    return errno || !*text || *end || !n || n>max?0:(unsigned)n;
}
int gu_mirror(struct gu_client *c,int argc,char **argv)
{
    unsigned port=5901,fps=5;int view_only=0;
    for(int i=0;i<argc;i++){
        if(!strcmp(argv[i],"--view-only"))view_only=1;
        else if(!strcmp(argv[i],"--port") && i+1<argc){port=number(argv[++i],65535);if(!port)return -1;}
        else if(!strcmp(argv[i],"--fps") && i+1<argc){fps=number(argv[++i],15);if(!fps)return -1;}
        else {fputs("mirror [--view-only] [--port 5901] [--fps 1..15]\n",stderr);return -1;}
    }
    if(!(c->caps&GU_CAP_MIRROR)){fputs("Update Glaze for screen mirroring support\n",stderr);return -1;}
    int listener=socket(AF_INET,SOCK_STREAM,0);
    if(listener<0)return -1;
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_port=htons((uint16_t)port);addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    int one=1;setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    if(bind(listener,(struct sockaddr *)&addr,sizeof(addr)) || listen(listener,1) || fcntl(listener,F_SETFL,O_NONBLOCK)<0){perror("VNC localhost listener");close(listener);return -1;}
    struct mirror_client *m=calloc(1,sizeof(*m));if(!m){close(listener);return -1;}m->client=c;
    struct rfb_source source={m,capture,input,keepalive};
    fprintf(stderr,"Glaze mirroring: vnc://127.0.0.1:%u (%s, up to %u fps)\n",port,view_only?"view only":"mouse and keyboard",fps);
    fputs("On macOS: open the URL in Screen Sharing. Ctrl-C stops the bridge.\n",stderr);
    /* Screen contents and typed text are not packet-traced. */
    int trace=c->usb->trace;c->usb->trace=0;
    int result=0;unsigned idle=0;
    while(!usb_cancelled){
        struct pollfd wait={.fd=listener,.events=POLLIN};int ready=poll(&wait,1,100);
        if(ready<0 && errno!=EINTR){result=-1;break;}
        if(ready>0 && (wait.revents&POLLIN)){
            int fd=accept(listener,NULL,NULL);if(fd<0)continue;
            if(fcntl(fd,F_SETFL,O_NONBLOCK)<0){close(fd);continue;}
            fputs("VNC viewer connected\n",stderr);
            int r=rfb_session(fd,&source,fps,view_only);
            close(fd);
            if(!usb_cancelled && gu_call(c,GU_MIRROR_CLOSE,NULL,0))m->failed=1;
            m->baseline=0;
            fputs("VNC viewer disconnected\n",stderr);
            if(r==-2 || m->failed){result=-1;break;}
            idle=0;
        }else if(++idle>=10){idle=0;if(keepalive(m)){result=-1;break;}}
    }
    c->usb->trace=trace;
    free(m);close(listener);return result;
}
