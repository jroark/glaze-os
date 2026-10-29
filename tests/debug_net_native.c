/* Production device TLS state machine on real native loopback TCP sockets. */
#include <glaze/debug_net.h>
#include <glaze/usb_wire.h>
#include <glaze/net.h>
#include <glaze/fs.h>
#include <glaze/prefs.h>
#include "../servers/usb/network.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
static const char *root;
uint32_t gz_time_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint32_t)(t.tv_sec*1000+t.tv_nsec/1000000); }
void gz_sleep_ms(uint32_t ms) { usleep(ms*1000); }
int gz_usb_ctl(uint32_t op,void *p,uint32_t n) { (void)op;(void)p;(void)n;return GZ_ENOENT; }
int pref_get(const char *key,void *p,uint32_t len)
{
    char path[512]; snprintf(path,sizeof path,"%s/%s",root,key);
    FILE *f=fopen(path,"rb"); if (!f) return GZ_ENOENT;
    int n=(int)fread(p,1,len,f); fclose(f); return n;
}
int pref_put(const char *key,const void *p,uint32_t len)
{
    char path[512]; snprintf(path,sizeof path,"%s/%s",root,key);
    FILE *f=fopen(path,"wb"); if (!f) return GZ_EIO;
    int n=(int)fwrite(p,1,len,f); int r=fclose(f); return r || n!=(int)len ? GZ_EIO : 0;
}
int fs_write_file(const char *path,const void *p,uint32_t len)
{ const char *name=strrchr(path,'/'); return pref_put(name+1,p,len)<0 ? GZ_EIO : (int)len; }
int fs_rename(const char *a,const char *b)
{ char x[512],y[512];snprintf(x,sizeof x,"%s/%s",root,strrchr(a,'/')+1);snprintf(y,sizeof y,"%s/%s",root,strrchr(b,'/')+1);return rename(x,y) ? GZ_EIO : 0; }
int net_status(struct net_status *s) { memset(s,0,sizeof *s);s->state=NET_UP;s->addr=NET_IP(127,0,0,1);return 0; }
int net_socket(int type) { (void)type;int fd=socket(AF_INET,SOCK_STREAM,0);if(fd>=0)fcntl(fd,F_SETFL,O_NONBLOCK);return fd; }
int net_sockopt(int fd,int option,int value)
{ return setsockopt(fd,option==NET_OPT_NODELAY?IPPROTO_TCP:SOL_SOCKET,option==NET_OPT_NODELAY?TCP_NODELAY:SO_REUSEADDR,&value,sizeof value); }
int net_bind(int fd,uint16_t port) { struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};return bind(fd,(struct sockaddr *)&a,sizeof a) ? GZ_EINUSE : port; }
int net_listen(int fd) { return listen(fd,4); }
int net_accept(int fd,uint32_t *addr,uint16_t *port,int timeout)
{ (void)addr;(void)port;(void)timeout;int s=accept(fd,NULL,NULL);if(s>=0)fcntl(s,F_SETFL,O_NONBLOCK);return s<0?GZ_ETIMEDOUT:s; }
int net_close(int s) { return close(s); }
int net_recv(int s,void *p,uint32_t n,int timeout)
{ (void)timeout;ssize_t r=recv(s,p,n,0);return r<0&&(errno==EAGAIN||errno==EWOULDBLOCK)?GZ_ETIMEDOUT:(int)r; }
int net_send_flags(int s,const void *p,uint32_t n,int flags)
{ (void)flags;ssize_t r=send(s,p,n,0);return r<0&&(errno==EAGAIN||errno==EWOULDBLOCK)?GZ_ETIMEDOUT:(int)r; }
int main(int argc,char **argv)
{
    if (argc!=2) return 2;
    signal(SIGPIPE,SIG_IGN);
    root=argv[1]; setvbuf(stdout,NULL,_IONBF,0);
    struct gu_parser parser={0}; uint8_t input[137],frame[GU_FRAME],reply[44]={0};
    puts("fixture: ready");
    for (;;) {
        int state=debug_net_poll(0);
        if (state<0) parser.used=0;
        int n=debug_net_read(input,sizeof input);
        if(n<0)parser.used=0;
        for(int i=0;i<n;i++)if(gu_feed(&parser,input[i])==1) {
            const uint8_t *f=parser.bytes;uint32_t op=gu_get32(f+4)>>16,id=gu_get32(f+8),len=gu_get32(f+12);
            unsigned size;
            if(op==GU_HELLO) {
                gu_put32(reply,1);gu_put32(reply+4,GU_CAP_NETWORK|GU_CAP_STATUS);gu_put32(reply+8,8192*1024);
                memcpy(reply+12,"native TLS fixture",18);size=gu_encode(frame,op|GU_REPLY,id,0,reply,44);
            } else if(op==GU_DEBUG_STATUS) {
                uint8_t status[8]={0};gu_put32(status,GU_DEBUG_CONNECTED|GU_DEBUG_NETWORK);
                size=gu_encode(frame,op|GU_REPLY,id,0,status,sizeof status);
            } else size=gu_encode(frame,op|GU_REPLY,id,0,f+GU_HEADER,len);
            unsigned sent=0;
            while(sent<size) { int r=debug_net_write(frame+sent,size-sent);if(r<0)break;sent+=(unsigned)r; }
        }
        usleep(1000);
    }
}
