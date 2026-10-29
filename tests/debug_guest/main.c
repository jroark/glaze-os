/* Test-only peer: real TLS over lwIP loopback, real development dispatcher.
 * Known fixture credentials never enter ordinary images or distributions. */
#include <glaze/debug_net.h>
#include <glaze/usb_wire.h>
#include <glaze/prefs.h>
#include <glaze/fs.h>
#include <glaze/net.h>
#include <glaze/sys.h>
#include <glaze/mirror.h>
#include <wolfssl/ssl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef DEBUG_RELOAD_ADDR
#define DEBUG_RELOAD_ADDR "10.0.0.254"
#endif
static int socket_id;
static WOLFSSL_CTX *ctx;
static WOLFSSL *tls;
static uint32_t ident;
static struct gu_parser parser;
static uint8_t reply[GU_PAYLOAD],data[GU_PAYLOAD];
static void check(int condition,const char *message)
{
    if (!condition) { printf("debug-net-guest: FAIL %s\n",message); exit(1); }
}
int glaze_tls_seed(unsigned char *out,unsigned n)
{ static unsigned count;for(unsigned i=0;i<n;i++)out[i]=(uint8_t)(++count*73);return 0; }
time_t glaze_tls_time(time_t *p) { if(p)*p=0;return 0; }
static unsigned key(WOLFSSL *ssl,const char *hint,char *identity,unsigned cap,unsigned char *out,unsigned len)
{ (void)ssl;(void)hint;if(cap<sizeof GD_IDENTITY||len<32)return 0;memcpy(identity,GD_IDENTITY,sizeof GD_IDENTITY);memset(out,0x21,32);return 32; }
static int rx(WOLFSSL *s,char *p,int n,void *arg)
{ (void)s;(void)arg;int r=net_recv(socket_id,p,n,0);return r==GZ_ETIMEDOUT?WOLFSSL_CBIO_ERR_WANT_READ:r>0?r:WOLFSSL_CBIO_ERR_CONN_CLOSE; }
static int tx(WOLFSSL *s,char *p,int n,void *arg)
{ (void)s;(void)arg;int r=net_send_flags(socket_id,p,n,NET_MSG_DONTWAIT);return r==GZ_ETIMEDOUT?WOLFSSL_CBIO_ERR_WANT_WRITE:r>0?r:WOLFSSL_CBIO_ERR_GENERAL; }
static void wait_tls(int r,uint32_t end)
{
    int e=wolfSSL_get_error(tls,r);
    if(e!=WOLFSSL_ERROR_WANT_READ && e!=WOLFSSL_ERROR_WANT_WRITE)printf("debug-net-guest: TLS error %d\n",e);
    check((e==WOLFSSL_ERROR_WANT_READ||e==WOLFSSL_ERROR_WANT_WRITE)&&(int32_t)(gz_time_ms()-end)<0,"TLS I/O");gz_sleep_ms(1);
}
static void open_peer(void)
{
    socket_id=net_socket(NET_TCP);check(socket_id>=0,"socket");
    struct net_status status; check(!net_status(&status),"local address");
    uint32_t end=gz_time_ms()+10000;
    while(net_connect(socket_id,status.addr,GD_PORT,200)<0) {
        net_close(socket_id);gz_sleep_ms(100);socket_id=net_socket(NET_TCP);
        check((int32_t)(gz_time_ms()-end)<0,"listener ready");
    }
    net_sockopt(socket_id,NET_OPT_NODELAY,1);
    ctx=wolfSSL_CTX_new(wolfTLSv1_2_client_method());check(ctx!=NULL,"TLS context");
    wolfSSL_CTX_set_psk_client_callback(ctx,key);check(wolfSSL_CTX_set_cipher_list(ctx,GD_CIPHER)==WOLFSSL_SUCCESS,"cipher");
    tls=wolfSSL_new(ctx);check(tls!=NULL,"TLS object");wolfSSL_SSLSetIORecv(tls,rx);wolfSSL_SSLSetIOSend(tls,tx);
    end=gz_time_ms()+10000;
    for(;;){int r=wolfSSL_connect(tls);if(r==WOLFSSL_SUCCESS)break;wait_tls(r,end);}
    parser.used=0;ident=0;
}
static void close_peer(void)
{ wolfSSL_free(tls);wolfSSL_CTX_free(ctx);net_close(socket_id);tls=NULL;ctx=NULL; }
static int rpc(uint32_t op,uint32_t id,const void *body,uint32_t len)
{
    uint8_t frame[GU_FRAME],incoming[257];unsigned size=gu_encode(frame,op,id,0,body,len),off=0;
    uint32_t end=gz_time_ms()+10000;
    while(off<size) { int r=wolfSSL_write(tls,frame+off,size-off);if(r>0)off+=r;else wait_tls(r,end); }
    while((int32_t)(gz_time_ms()-end)<0) {
        int n=wolfSSL_read(tls,incoming,sizeof incoming);
        if(n<=0){wait_tls(n,end);continue;}
        for(int i=0;i<n;i++)if(gu_feed(&parser,incoming[i])==1) {
            const uint8_t *p=parser.bytes;
            check(gu_get32(p+8)==id && (gu_get32(p+4)>>16)==(op|GU_REPLY),"reply id");
            if((int)gu_get32(p+16)<0)printf("debug-net-guest: op %u status %d\n",op,(int)gu_get32(p+16));
            check((int)gu_get32(p+16)>=0,"RPC status");
            unsigned n=gu_get32(p+12);memcpy(reply,p+GU_HEADER,n);return n;
        }
    }
    check(0,"RPC timeout");return -1;
}
static int call(uint32_t op,const void *body,uint32_t n) { return rpc(op,++ident,body,n); }
static void event(uint8_t *p,unsigned ms,unsigned type,unsigned code,int x,int y)
{ gu_put32(p,ms);p[4]=type;p[5]=type>>8;p[6]=code;p[7]=code>>8;p[8]=x;p[9]=x>>8;p[10]=y;p[11]=y>>8; }
static void script(int unlock)
{
    unsigned n=unlock?6:1;gu_put32(data,n);check(call(GU_SCRIPT_BEGIN,data,4)==0,"script begin");
    gu_put32(data,0);
    if(unlock)for(unsigned i=0;i<n;i++)event(data+4+i*12,i*100,i==0?GZ_EV_TOUCH_DOWN:i==5?GZ_EV_TOUCH_UP:GZ_EV_TOUCH_MOVE,0,40+(int)i*39,280);
    else event(data+4,0,GZ_EV_TOUCH_DOWN,0,120,120);
    check(call(GU_SCRIPT_DATA,data,4+n*12)==0,"script events");check(call(GU_SCRIPT_START,NULL,0)==0,"script start");
    uint32_t end=gz_time_ms()+5000;
    do {gz_sleep_ms(100);check(call(GU_SCRIPT_STATUS,NULL,0)==16,"script status");check((int32_t)(gz_time_ms()-end)<0,"script finish");}while(gu_get32(reply)!=GU_SCRIPT_DONE);
    check(gu_get32(reply+8)==n,"script event count");
}
static void exact(int fd,void *p,unsigned len)
{ unsigned done=0;while(done<len){int n=net_recv(fd,(uint8_t *)p+done,len-done,10000);check(n>0,"image stream");done+=n;} }
static void reload(void)
{
    uint32_t address;check(!net_aton(DEBUG_RELOAD_ADDR,&address),"reload address");
    int fd=net_socket(NET_TCP);check(fd>=0&&!net_connect(fd,address,8877,5000),"reload fixture connection");
    exact(fd,data,24);uint32_t kernel=gu_get32(data),romfs=gu_get32(data+4),total=((kernel+4095)&~4095u)+romfs;
    check(call(GU_BOOT_BEGIN,data,24)==0,"reload begin");
    for(unsigned off=0;off<total;) {
        unsigned n=total-off>GU_PAYLOAD-4?GU_PAYLOAD-4:total-off;
        gu_put32(data,off);exact(fd,data+4,n);check(call(GU_BOOT_DATA,data,n+4)==0,"reload data");off+=n; if(off%(512*1024)<n)printf("debug-net-guest: reload staged %u/%u KiB\n",off/1024,total/1024);
    }
    net_close(fd);check(call(GU_BOOT_COMMIT,NULL,0)==0,"reload commit");
    puts("debug-net-guest: reload accepted");
    for(;;)gz_sleep_ms(1000);
}
static uint8_t mirror_image[GU_SCREEN_BYTES];
static void mirror_capture(unsigned flags)
{
    gu_put32(data,flags);
    check(call(GU_MIRROR_NEXT,data,4)==24,"mirror metadata");
    unsigned count=gu_get32(reply+12),total=gu_get32(reply+16);uint32_t crc=gu_get32(reply+20);
    check(gu_get32(reply)==GM_WIDTH && gu_get32(reply+4)==GM_HEIGHT && gu_get32(reply+8)==GM_TILE &&
          count<=GM_TILES && total==count*GM_RECORD,"mirror layout");
    if(flags&GM_FORCE)check(count==GM_TILES,"full mirror frame");
    uint8_t record[GM_RECORD],seen[GM_TILES]={0};unsigned used=0,records=0;
    for(unsigned off=0;off<total;){
        gu_put32(data,off);int n=call(GU_MIRROR_READ,data,4);check(n>0 && (unsigned)n<=total-off,"mirror read");
        for(int i=0;i<n;i++){
            record[used++]=reply[i];if(used!=GM_RECORD)continue;
            unsigned tile=gu_get32(record);check(tile<GM_TILES && !seen[tile],"unique mirror tile");seen[tile]=1;
            unsigned start=((tile/GM_COLUMNS)*GM_TILE*GM_WIDTH+(tile%GM_COLUMNS)*GM_TILE)*2;
            for(unsigned y=0;y<GM_TILE;y++)memcpy(mirror_image+start+y*GM_WIDTH*2,record+4+y*GM_TILE*2,GM_TILE*2);
            used=0;records++;
        }
        off+=(unsigned)n;
    }
    check(!used && records==count && ~gu_crc(~0u,mirror_image,sizeof(mirror_image))==crc,"mirror frame CRC");
}
static void mirror_test(void)
{
    mirror_capture(GM_FORCE);gz_sleep_ms(100);mirror_capture(0);
    check(call(GU_DEBUG_STATUS,NULL,0)==8 && (gu_get32(reply)&GU_DEBUG_MIRROR),"mirror activity state");
    memset(data,0,24);data[0]=GZ_EV_TOUCH_DOWN;data[4]=239;data[6]=63;data[7]=1;
    data[8]=GZ_EV_TOUCH_MOVE;data[12]=238;data[14]=62;data[15]=1;
    data[16]=GZ_EV_TOUCH_UP;data[20]=238;data[22]=62;data[23]=1;
    check(!call(GU_MIRROR_INPUT,data,24),"immediate mirror touch input");
    gz_sleep_ms(100);mirror_capture(0);
    check(!call(GU_MIRROR_CLOSE,NULL,0),"mirror close");
    check(call(GU_DEBUG_STATUS,NULL,0)==8 && !(gu_get32(reply)&GU_DEBUG_MIRROR),"mirror activity cleared");
    check(call(GU_SCRIPT_STATUS,NULL,0)==16 && gu_get32(reply)==GU_SCRIPT_EMPTY,"mirror input released");
    puts("debug-net-guest: mirror deltas/CRC, immediate input and close PASS");
}
int main(void)
{
    /* The production network transport is off unless explicitly provisioned. */
    int ram=gz_boot_mode()==GZ_BOOT_MODE_CE;
    uint8_t config[GD_CONFIG_BYTES];int32_t on=1;
    if(!ram) {
        memset(config,0x21,32);memset(config+32,0x57,32);
        int port; uint32_t until=gz_time_ms()+10000;
        while ((port=gz_port_lookup(GD_PAIR_PORT))<=0) { check((int32_t)(gz_time_ms()-until)<0,"pairing service ready"); gz_sleep_ms(20); }
        struct gz_msg request,response; memcpy(&request,config,sizeof config);
        check(!gz_send(port,&request,&response) && !response.w[0],"serialized key import");
        check(!pref_put(GD_ENABLED,&on,4),"fixture pairing enable");
    } else {
        uint32_t end=gz_time_ms()+10000;
        while(pref_get(GD_CONFIG,config,sizeof config)!=sizeof config){check((int32_t)(gz_time_ms()-end)<0,"restored pairing");gz_sleep_ms(100);}
        for(unsigned i=0;i<32;i++)check(config[i]==0x21,"preserved PSK");
        check(pref_get(GD_ENABLED,&on,4)==4 && on==1,"preserved enabled state");
    }
    check(gz_spawn("bin/springboard",GZ_SPAWN_PRIVILEGED)>0,"SpringBoard spawn");
    check(!net_wait_up(15000),"network up");
    check(wolfSSL_Init()==WOLFSSL_SUCCESS,"TLS init");open_peer();
    check(call(GU_HELLO,NULL,0)==44,"hello");
    unsigned caps=gu_get32(reply+4);check((caps&1023u)==1023u,"USB capability parity");
    check(caps&GU_CAP_MIRROR,"mirror capability");
    if(ram){puts("debug-net-guest: ALL PASS (RAM reload, pairing restored, TLS reconnected)");for(;;)gz_sleep_ms(1000);}
    for(unsigned i=0;i<3000;i++)data[8+i]=(uint8_t)(i*17);
    uint32_t sum=~gu_crc(~0u,data+8,3000);gu_put32(data,3000);gu_put32(data+4,sum);
    const char *path="/Documents/network-debug.dat";memcpy(data+8,path,strlen(path));
    check(!call(GU_PUT_BEGIN,data,8+strlen(path)),"upload begin");
    gu_put32(data,0);for(unsigned i=0;i<3000;i++)data[4+i]=(uint8_t)(i*17);
    check(!call(GU_PUT_DATA,data,3004),"upload data");check(!rpc(GU_PUT_DATA,ident,data,3004),"duplicate write reply");
    check(!call(GU_PUT_END,NULL,0),"upload end");
    gu_put32(data,0);memcpy(data+4,path,strlen(path));check(call(GU_GET,data,4+strlen(path))==3000,"download length");
    for(unsigned i=0;i<3000;i++)check(reply[i]==(uint8_t)(i*17),"file round trip");
    check(call(GU_LOG_READ,NULL,0)>=4,"console logs");
    script(1);gz_sleep_ms(500);
    check(!call(GU_RUN,"bin/settings",12),"app launch");
    check(call(GU_DEBUG_STATUS,NULL,0)==8 && (gu_get32(reply)&GU_DEBUG_NETWORK) && gu_get32(reply+4)>0,"network badge/app status");
    check(call(GU_SCREEN_BEGIN,NULL,0)==16 && gu_get32(reply)==240 && gu_get32(reply+4)==320,"screenshot dimensions");
    sum=~0u;
    for(unsigned off=0;off<GU_SCREEN_BYTES;) {gu_put32(data,off);int n=call(GU_SCREEN_READ,data,4);check(n>0,"screenshot read");sum=gu_crc(sum,reply,n);off+=n;}
    check(~sum!=0,"screenshot pixels");check(!call(GU_SCREEN_END,NULL,0),"screenshot end");
    mirror_test();
    script(0);close_peer();gz_sleep_ms(1500);open_peer();check(call(GU_HELLO,NULL,0)==44,"reconnect");
    check(call(GU_SCRIPT_STATUS,NULL,0)==16 && gu_get32(reply)==GU_SCRIPT_EMPTY,"disconnect cancels held input");
    puts("debug-net-guest: files, replay, logs, app, screenshot, script, disconnect PASS");
    reload();return 0;
}
