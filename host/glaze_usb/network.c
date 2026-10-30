/* TLS 1.2 PSK client for the exact same development RPC client as USB. */
#include "network.h"
#include <sys/random.h>
#include <glaze/debug_net.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct network { int fd; WOLFSSL_CTX *ctx; WOLFSSL *tls; uint8_t key[32]; };
static uint64_t now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+(unsigned)t.tv_nsec/1000000;
}
static void erase(void *p,size_t n) { volatile uint8_t *b=p; while (n--) *b++=0; }
int glaze_tls_seed(unsigned char *p,unsigned n) { return getentropy(p,n) ? -1 : 0; }
time_t glaze_tls_time(time_t *p) { return time(p); }
static int load_key(const char *path,uint8_t *out)
{
    if (!path) { fputs("Network debugging requires --key KEYFILE.\n",stderr); return -1; }
    int fd=open(path,O_RDONLY); if (fd<0) { perror(path); return -1; }
    struct stat st; uint8_t b[GD_CONFIG_BYTES+1]; size_t used=0;
    int r=fstat(fd,&st);
    if (r || !S_ISREG(st.st_mode) || (st.st_mode&077)) {
        close(fd); fputs("Pairing key must be a private regular file (chmod 600).\n",stderr); return -1;
    }
    while (used<sizeof b) {
        ssize_t n=read(fd,b+used,sizeof b-used);
        if (n<0 && errno==EINTR) continue;
        if (n<0) { r=-1; break; }
        if (!n) break;
        used+=(size_t)n;
    }
    close(fd);
    if (r || used!=GD_CONFIG_BYTES) { fputs("Invalid pairing key file.\n",stderr); r=-1; }
    else { if (out) memcpy(out,b,32); r=0; }
    erase(b,sizeof b); return r;
}
int debug_host_key_valid(const char *path) { return load_key(path,NULL); }
int debug_host_keygen(const char *path)
{
    uint8_t key[GD_CONFIG_BYTES];
    if (getentropy(key,sizeof key)) { perror("OS CSPRNG"); return -1; }
    int fd=open(path,O_CREAT|O_EXCL|O_WRONLY,0600);
    if (fd<0) { perror(path); erase(key,sizeof key); return -1; }
    size_t done=0; int r=0;
    while (done<sizeof key) {
        ssize_t n=write(fd,key+done,sizeof key-done);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { r=-1; break; }
        done+=(size_t)n;
    }
    if (close(fd)) r=-1;
    if (r) unlink(path);
    erase(key,sizeof key); return r;
}
static unsigned psk(WOLFSSL *ssl,const char *hint,char *identity,unsigned ident_len,unsigned char *key,unsigned capacity)
{
    (void)hint;
    struct network *n=wolfSSL_GetIOReadCtx(ssl);
    if (!n || ident_len<sizeof GD_IDENTITY || capacity<sizeof n->key) return 0;
    memcpy(identity,GD_IDENTITY,sizeof GD_IDENTITY); memcpy(key,n->key,sizeof n->key); return sizeof n->key;
}
static int rx(WOLFSSL *ssl,char *buf,int len,void *arg)
{
    (void)ssl; struct network *n=arg;
    ssize_t r=recv(n->fd,buf,(size_t)len,0);
    if (r<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) return WOLFSSL_CBIO_ERR_WANT_READ;
    return r>0 ? (int)r : r==0 ? WOLFSSL_CBIO_ERR_CONN_CLOSE : WOLFSSL_CBIO_ERR_GENERAL;
}
static int tx(WOLFSSL *ssl,char *buf,int len,void *arg)
{
    (void)ssl; struct network *n=arg;
    ssize_t r=send(n->fd,buf,(size_t)len,0);
    if (r<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) return WOLFSSL_CBIO_ERR_WANT_WRITE;
    return r>0 ? (int)r : WOLFSSL_CBIO_ERR_GENERAL;
}
static int wait_io(struct network *n,int error,uint64_t end)
{
    if (error!=WOLFSSL_ERROR_WANT_READ && error!=WOLFSSL_ERROR_WANT_WRITE) return -1;
    if (usb_cancelled || now()>=end) return -1;
    int left=(int)(end-now()); if (left>50) left=50;
    struct pollfd p={n->fd,error==WOLFSSL_ERROR_WANT_READ ? POLLIN : POLLOUT,0};
    int r=poll(&p,1,left);
    return r<0 && errno!=EINTR ? -1 : 0;
}
void debug_host_close(struct usb_link *l)
{
    struct network *n=l->network;
    if (!n) return;
    /* close(), rather than a blocking TLS shutdown, also cancels gestures. */
    if (n->tls) wolfSSL_free(n->tls);
    if (n->ctx) wolfSSL_CTX_free(n->ctx);
    if (n->fd>=0) close(n->fd);
    erase(n,sizeof *n); free(n); l->network=NULL;
}
int debug_host_open(struct usb_link *l,const char *address,const char *keyfile)
{
    struct network *n=calloc(1,sizeof *n); if (!n) return -1;
    n->fd=-1; l->network=n;
    if (load_key(keyfile,n->key)) goto fail;
    char host[256],service[8];
    if (!address || strlen(address)>=sizeof host) goto fail;
    strcpy(host,address); snprintf(service,sizeof service,"%u",GD_PORT);
    char *colon=strchr(host,':');
    if (colon) {
        char *end; unsigned long port=strtoul(colon+1,&end,10);
        if (*end || !port || port>65535) goto fail;
        *colon=0; snprintf(service,sizeof service,"%lu",port);
    }
    if (!*host) goto fail;
    struct addrinfo hints={0},*result=NULL; hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
    if (getaddrinfo(host,service,&hints,&result)) goto fail;
    for (struct addrinfo *a=result;a;a=a->ai_next) {
        n->fd=socket(a->ai_family,a->ai_socktype,a->ai_protocol);
        if (n->fd<0) continue;
        int flags=fcntl(n->fd,F_GETFL,0); if (flags<0 || fcntl(n->fd,F_SETFL,flags|O_NONBLOCK)<0) { close(n->fd); n->fd=-1; continue; }
        int r=connect(n->fd,a->ai_addr,a->ai_addrlen);
        if (r && errno==EINPROGRESS) {
            struct pollfd p={n->fd,POLLOUT,0};
            r=poll(&p,1,3000);
            int e=0; socklen_t sz=sizeof e;
            r=r>0 && !getsockopt(n->fd,SOL_SOCKET,SO_ERROR,&e,&sz) && !e ? 0 : -1;
        }
        if (!r) break;
        close(n->fd); n->fd=-1;
    }
    freeaddrinfo(result); if (n->fd<0) goto fail;
    int one=1; setsockopt(n->fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof one);
    if (wolfSSL_Init()!=WOLFSSL_SUCCESS || !(n->ctx=wolfSSL_CTX_new(wolfTLSv1_2_client_method()))) goto fail;
    wolfSSL_CTX_set_psk_client_callback(n->ctx,psk);
    if (wolfSSL_CTX_set_cipher_list(n->ctx,GD_CIPHER)!=WOLFSSL_SUCCESS || !(n->tls=wolfSSL_new(n->ctx))) goto fail;
    wolfSSL_SetIOReadCtx(n->tls,n); wolfSSL_SetIOWriteCtx(n->tls,n);
    wolfSSL_SSLSetIORecv(n->tls,rx); wolfSSL_SSLSetIOSend(n->tls,tx);
    uint64_t end=now()+5000;
    for (;;) {
        int r=wolfSSL_connect(n->tls);
        if (r==WOLFSSL_SUCCESS) break;
        if (wait_io(n,wolfSSL_get_error(n->tls,r),end)) goto fail;
    }
    l->revision=0x0100;
    fprintf(stderr,"Paired TLS debugger connected to %s\n",address); return 0;
fail:
    fputs("Network debugger connection failed (check address, key and device setting).\n",stderr);
    debug_host_close(l); return -1;
}
int debug_host_read(struct usb_link *l,void *buf,size_t len,unsigned timeout)
{
    struct network *n=l->network; uint64_t end=now()+timeout;
    for (;;) {
        int r=wolfSSL_read(n->tls,buf,(int)len);
        if (r>0) return r;
        int e=wolfSSL_get_error(n->tls,r);
        if (e!=WOLFSSL_ERROR_WANT_READ && e!=WOLFSSL_ERROR_WANT_WRITE) return LIBUSB_ERROR_IO;
        if (wait_io(n,e,end)) return usb_cancelled ? LIBUSB_ERROR_INTERRUPTED : LIBUSB_ERROR_TIMEOUT;
    }
}
int debug_host_write(struct usb_link *l,const void *buf,size_t len,unsigned timeout)
{
    struct network *n=l->network; uint64_t end=now()+timeout;
    while (len) {
        int r=wolfSSL_write(n->tls,buf,(int)len);
        if (r>0) { buf=(const uint8_t *)buf+r; len-=(size_t)r; continue; }
        if (wait_io(n,wolfSSL_get_error(n->tls,r),end)) return LIBUSB_ERROR_IO;
    }
    return 0;
}
