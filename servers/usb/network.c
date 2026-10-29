/* Paired TLS 1.2 PSK development transport; no unauthenticated dispatcher.
 * Same GZUB requests as USB, inside authenticated TLS records. */
#include "network.h"
#include <glaze/debug_net.h>
#include <glaze/usb.h>
#include <glaze/net.h>
#include <glaze/fs.h>
#include <glaze/prefs.h>
#include <glaze/wifi.h>
#include <stdio.h>
#include <string.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssl/wolfcrypt/random.h>
#if !defined(__mips__)
extern uint32_t gz_time_ms(void);
extern void gz_sleep_ms(uint32_t);
extern int gz_usb_ctl(uint32_t,void *,uint32_t);
#endif
static WOLFSSL_CTX *ctx;
static WOLFSSL *tls;
static int listener=-1, peer=-1, authenticated, enabled, initialized;
static uint32_t check_at, deadline, retry_at, address;
static uint8_t key[32];
static void erase(void *p,unsigned n) { volatile uint8_t *b=p; while (n--) *b++=0; }

/* A host CSPRNG supplies the pairing record. Advance its private seed on
 * durable storage BEFORE releasing bytes; clocks never substitute for it.
 * This seed is separate from Wi-Fi's concurrently updated entropy ratchet. */
int glaze_tls_seed(unsigned char *out,unsigned n)
{
    uint8_t config[GD_CONFIG_BYTES+1], input[64], block[32];
    int rv=-1;
    while (n) {
        if (pref_get(GD_CONFIG,config,sizeof config)!=GD_CONFIG_BYTES) goto done;
        memcpy(input,config+32,32);
        memset(input+32,0,32); memcpy(input+32,"Glaze debug RNG output v1",25);
        if (wc_Sha256Hash(input,sizeof input,block)) goto done;
        memset(input+32,0,32); memcpy(input+32,"Glaze debug RNG ratchet v1",26);
        if (wc_Sha256Hash(input,sizeof input,config+32)) goto done;
        const char *tmp=PREFS_DIR "/debug.network.tmp";
        if (fs_write_file(tmp,config,GD_CONFIG_BYTES)!=GD_CONFIG_BYTES ||
            fs_rename(tmp,PREFS_DIR "/" GD_CONFIG)<0) goto done;
        unsigned k=n<sizeof block ? n : sizeof block;
        memcpy(out,block,k); out+=k; n-=k;
    }
    rv=0;
done:
    erase(config,sizeof config); erase(input,sizeof input); erase(block,sizeof block);
    return rv;
}
time_t glaze_tls_time(time_t *p) { if (p) *p=0; return 0; } /* PSK has no certificate dates. */
static unsigned psk(WOLFSSL *ssl,const char *identity,unsigned char *out,unsigned capacity)
{
    (void)ssl;
    if (!identity || strcmp(identity,GD_IDENTITY) || capacity<sizeof key) return 0;
    memcpy(out,key,sizeof key); return sizeof key;
}
static int receive(WOLFSSL *ssl,char *buf,int n,void *arg)
{
    (void)ssl; (void)arg;
    int r=net_recv(peer,buf,(unsigned)n,0);
    if (r==GZ_ETIMEDOUT) return WOLFSSL_CBIO_ERR_WANT_READ;
    return r>0 ? r : r==0 ? WOLFSSL_CBIO_ERR_CONN_CLOSE : WOLFSSL_CBIO_ERR_GENERAL;
}
static int transmit(WOLFSSL *ssl,char *buf,int n,void *arg)
{
    (void)ssl; (void)arg;
    int r=net_send_flags(peer,buf,(unsigned)n,NET_MSG_DONTWAIT);
    if (r==GZ_ETIMEDOUT) return WOLFSSL_CBIO_ERR_WANT_WRITE;
    return r>0 ? r : WOLFSSL_CBIO_ERR_GENERAL;
}
void debug_net_drop(void)
{
    authenticated=0;
    if (tls) { wolfSSL_free(tls); tls=NULL; }
    if (peer>=0) { net_close(peer); peer=-1; }
    retry_at=gz_time_ms()+1000;
}
static void stop(void)
{
    debug_net_drop();
    if (listener>=0) { net_close(listener); listener=-1; }
    if (ctx) { wolfSSL_CTX_free(ctx); ctx=NULL; }
}
int debug_net_active(void) { return authenticated; }
int debug_net_poll(int busy)
{
    uint32_t now=gz_time_ms();
    if (!initialized) { check_at=retry_at=now; initialized=1; }
    int was=authenticated;
    if ((int32_t)(now-check_at)>=0) {
        uint8_t config[GD_CONFIG_BYTES+1]; int32_t on=0;
        if (pref_get(GD_ENABLED,&on,sizeof on)!=sizeof on || on!=1 ||
            pref_get(GD_CONFIG,config,sizeof config)!=GD_CONFIG_BYTES) on=0;
        struct net_status st={0};
        if (on && (net_status(&st)<0 || (st.state!=NET_UP && st.state!=NET_UP_STATIC))) on=0;
        if (!on || !enabled || memcmp(key,config,sizeof key) || address!=st.addr) {
            stop();
            if (on) { memcpy(key,config,sizeof key); address=st.addr; }
            else erase(key,sizeof key);
        }
        enabled=on; erase(config,sizeof config); check_at=now+500;
    }
    if (busy && peer>=0) debug_net_drop();
    if (was && !authenticated) return -1;
    if (!enabled || busy || (peer<0 && (int32_t)(now-retry_at)<0)) return 0;
    if (!ctx) {
        if (wolfSSL_Init()!=WOLFSSL_SUCCESS || !(ctx=wolfSSL_CTX_new(wolfTLSv1_2_server_method()))) return 0;
        wolfSSL_CTX_set_verify(ctx,WOLFSSL_VERIFY_NONE,NULL);
        wolfSSL_CTX_set_psk_server_callback(ctx,psk);
        if (wolfSSL_CTX_set_cipher_list(ctx,GD_CIPHER)!=WOLFSSL_SUCCESS) { stop(); return 0; }
    }
    if (listener<0) {
        listener=net_socket(NET_TCP);
        if (listener<0) return 0;
        net_sockopt(listener,NET_OPT_REUSEADDR,1);
        if (net_bind(listener,GD_PORT)<0 || net_listen(listener)<0) { stop(); return 0; }
        puts("debug: paired TLS listener ready on port 3000");
    }
    if (peer<0 && (int32_t)(now-retry_at)>=0) {
        peer=net_accept(listener,NULL,NULL,0);
        if (peer<0) { int error=peer; peer=-1; if (error!=GZ_ETIMEDOUT) stop(); return 0; }
        net_sockopt(peer,NET_OPT_NODELAY,1);
        tls=wolfSSL_new(ctx);
        if (!tls) { debug_net_drop(); return 0; }
        wolfSSL_SSLSetIORecv(tls,receive); wolfSSL_SSLSetIOSend(tls,transmit);
        deadline=now+5000;
    }
    if (tls && !authenticated) {
        int r=wolfSSL_accept(tls);
        if (r==WOLFSSL_SUCCESS) { authenticated=1; puts("debug: paired network session open"); }
        else {
            int e=wolfSSL_get_error(tls,r);
            if ((e!=WOLFSSL_ERROR_WANT_READ && e!=WOLFSSL_ERROR_WANT_WRITE) ||
                (int32_t)(now-deadline)>=0) debug_net_drop();
        }
    }
    return authenticated;
}
int debug_net_read(void *buf,unsigned n)
{
    if (!authenticated) return 0;
    int r=wolfSSL_read(tls,buf,(int)n);
    if (r>0) return r;
    int e=wolfSSL_get_error(tls,r);
    if (e==WOLFSSL_ERROR_WANT_READ || e==WOLFSSL_ERROR_WANT_WRITE) return 0;
    debug_net_drop(); return -1;
}
int debug_net_write(const void *buf,unsigned n)
{
    if (!authenticated) return GZ_EDEAD;
    uint32_t end=gz_time_ms()+3000;
    for (;;) {
        int r=wolfSSL_write(tls,buf,(int)n);
        if (r>0) return r;
        int e=wolfSSL_get_error(tls,r);
        if ((e!=WOLFSSL_ERROR_WANT_READ && e!=WOLFSSL_ERROR_WANT_WRITE) ||
            (int32_t)(gz_time_ms()-end)>=0) { debug_net_drop(); return GZ_EDEAD; }
        gz_sleep_ms(1);
    }
}
void debug_net_restore(void)
{
    struct gd_boot b={0};
    int n=gz_usb_ctl(GZ_USB_DEBUG_RESTORE,&b,sizeof b);
    if (n==sizeof b && b.version==1 && b.enabled==1 && b.wifi_size<=sizeof b.wifi) {
        int rv=pref_put(GD_CONFIG,b.config,sizeof b.config);
        if (!rv) rv=pref_put("net.entropy",b.entropy,sizeof b.entropy);
        if (!rv && b.wifi_size) rv=pref_put(WIFI_PREF,b.wifi,b.wifi_size);
        if (!rv) rv=pref_put(GD_ENABLED,&b.enabled,sizeof b.enabled);
        if (rv) puts("debug: could not restore paired network configuration");
    }
    erase(&b,sizeof b);
}
int debug_net_handoff(void)
{
    struct gd_boot b={.version=1};
    int32_t on=0;
    if (pref_get(GD_ENABLED,&on,4)!=4 || on!=1) return 0;
    if (pref_get(GD_CONFIG,b.config,sizeof b.config)!=sizeof b.config) return GZ_EIO;
    WC_RNG rng;
    if (wc_InitRng(&rng)) { erase(&b,sizeof b); return GZ_EIO; }
    int rv=wc_RNG_GenerateBlock(&rng,b.config+32,32);
    if (!rv) rv=wc_RNG_GenerateBlock(&rng,b.entropy,sizeof b.entropy);
    wc_FreeRng(&rng);
    int n=pref_get(WIFI_PREF,b.wifi,sizeof b.wifi);
    if (n>0) b.wifi_size=(unsigned)n;
    b.enabled=1;
    if (!rv) rv=gz_usb_ctl(GZ_USB_RELOAD_DEBUG,&b,sizeof b);
    erase(&b,sizeof b);
    return rv;
}
