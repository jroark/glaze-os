/* Production supplicant vs independent OpenSSL authenticator, plus published
 * crypto vectors. No connection can complete by an emulator success shortcut. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wifi_supplicant.h"
#include "utils/includes.h"
#include "utils/common.h"
#include "crypto/sha1.h"
#include "crypto/aes_wrap.h"
#include "hw/wifi_security.h"
static wifi_security_t ap;
static unsigned installs, authorizations, failures, rng_fail;
static uint32_t now;
static uint8_t latest[512];static unsigned latest_len;
static int send_frame(const uint8_t *p,unsigned n) { wifi_security_tx(&ap,p,n);return n; }
static int install(const struct wifi_key *key)
{
    installs++;

    return wifi_security_key(&ap,key->pairwise,key->cipher,key->key,key->length)?0:-1;
}
static int authorize(void) { assert(ap.pairwise && ap.group);authorizations++;return 0; }
static void failed(int error) { assert(error<0);failures++; }
static int random_bytes(uint8_t *p,unsigned n) { if(rng_fail)return -1;for(unsigned i=0;i<n;i++)p[i]=0x60+i;return 0; }
static uint32_t time_ms(void) { return now; }
static const struct wifi_supplicant_io io={send_frame,install,authorize,failed,random_bytes,time_ms};
static struct wifi_credentials credentials(unsigned mode)
{
    struct wifi_credentials c={0};c.network.ssid_len=5;memcpy(c.network.ssid,"BE300",5);
    c.security=mode;c.key_len=8;memcpy(c.key,"password",8);return c;
}
static void begin(unsigned mode,const char *password)
{
    wifi_supplicant_stop();installs=authorizations=failures=rng_fail=now=0;
    const uint8_t mac[]={2,3,4,5,6,7};wifi_security_init(&ap,mac);
    assert(wifi_security_configure(&ap,mode,password));
    struct wifi_ap network={0};memcpy(network.bssid,ap.bssid,6);network.ie_len=ap.ie_len;
    memcpy(network.ie,ap.ie,ap.ie_len);
    struct wifi_credentials c=credentials(mode);struct wifi_config config;
    assert(!wifi_supplicant_prepare(&io,&c,&network,mac,&config));
    assert(config.ie_len==ap.ie_len && !memcmp(config.ie,ap.ie,ap.ie_len));
    wifi_supplicant_associated();wifi_security_start(&ap);
}
static void receive(void)
{
    latest_len=wifi_security_pop(&ap,latest,sizeof(latest));assert(latest_len);
    wifi_supplicant_rx(latest,latest_len);
}
static void vectors(void)
{
    uint8_t pmk[32],expected[32];
    assert(!pbkdf2_sha1("password",(const uint8_t *)"IEEE",4,4096,pmk,sizeof(pmk)));
    assert(!hexstr2bin("f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e",expected,32));
    assert(!memcmp(pmk,expected,32));
    uint8_t key[16],plain[16],wrapped[24],out[16],cipher[24];
    assert(!hexstr2bin("000102030405060708090a0b0c0d0e0f",key,16));
    assert(!hexstr2bin("00112233445566778899aabbccddeeff",plain,16));
    assert(!hexstr2bin("1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5",cipher,24));
    assert(!aes_wrap(key,16,2,plain,wrapped));assert(!memcmp(wrapped,cipher,24));
    assert(!aes_unwrap(key,16,2,cipher,out));assert(!memcmp(out,plain,16));
    cipher[4]^=1;assert(aes_unwrap(key,16,2,cipher,out)<0);
}
static void validation(void)
{
    struct wifi_credentials c=credentials(WIFI_WEP);uint8_t out[13];
    assert(wifi_credentials_validate(&c)==GZ_EINVAL);
    c.key_len=10;memcpy(c.key,"0102030405",10);assert(wifi_wep_decode(&c,out)==5 && out[4]==5);
    for(unsigned slot=0;slot<4;slot++){c.key_index=slot;assert(!wifi_credentials_validate(&c));}
    c.key_index=4;assert(wifi_credentials_validate(&c)==GZ_EINVAL);c.key_index=0;
    c.key[2]='x';assert(wifi_credentials_validate(&c)==GZ_EINVAL);
    c=credentials(WIFI_WPA2_PSK);c.key_len=7;assert(wifi_credentials_validate(&c)==GZ_EINVAL);
    c.key_len=64;memset(c.key,'a',64);assert(!wifi_credentials_validate(&c));
    c.key[63]='z';assert(wifi_credentials_validate(&c)==GZ_EINVAL);
    struct { uint32_t version, reconnect; struct wifi_request network; } legacy={1,1,{5,"BE300"}};
    struct wifi_preference saved;
    assert(!wifi_preference_decode(&legacy,sizeof(legacy),&saved));
    assert(saved.version==2 && saved.reconnect && saved.credentials.security==WIFI_OPEN);
    assert(wifi_preference_decode(&legacy,sizeof(legacy)-1,&saved)==GZ_EINVAL);
    legacy.reconnect=2;assert(wifi_preference_decode(&legacy,sizeof(legacy),&saved)==GZ_EINVAL);
    struct wifi_preference current={.version=2,.reconnect=1,.credentials=credentials(WIFI_WPA2_PSK)};
    assert(!wifi_preference_decode(&current,sizeof(current),&saved));
    current.credentials.key_len=65;assert(wifi_preference_decode(&current,sizeof(current),&saved)==GZ_EINVAL);
    assert(!saved.credentials.key_len);
    c=credentials(WIFI_WPA_PSK);c.network.ssid_len=33;assert(wifi_credentials_validate(&c)==GZ_EINVAL);
}
int main(void)
{
    vectors();validation();
    for(unsigned mode=WIFI_WPA_PSK;mode<=WIFI_WPA2_PSK;mode++){
        begin(mode,"password");
        receive();assert(ap.stage==2 && !installs && !authorizations);
        receive();assert(ap.stage==(mode==WIFI_WPA_PSK?3u:4u));
        if(mode==WIFI_WPA_PSK)receive();
        assert(ap.authorized && installs==2 && authorizations==1 && !failures);
        unsigned count=installs;wifi_supplicant_rx(latest,latest_len);assert(installs==count);
        uint8_t data[60]={0};assert(wifi_security_tx(&ap,data,sizeof(data)));
        assert(wifi_security_rekey(&ap));receive();
        assert(ap.authorized && ap.group && installs==count+1 && !failures);
        count=installs;wifi_supplicant_rx(latest,latest_len);assert(installs==count);
        wifi_supplicant_stop();assert(wifi_supplicant_state()==WIFI_DISCONNECTED);
        begin(mode,"wrongkey");receive();assert(!ap.pending_len);
        assert(!installs && !authorizations && !ap.authorized);
        now=16000;wifi_supplicant_poll();assert(failures==1 && wifi_supplicant_state()==WIFI_EAUTH);
        begin(mode,"password");receive();
        latest_len=wifi_security_pop(&ap,latest,sizeof(latest));assert(latest_len);
        latest[14+81]^=1;wifi_supplicant_rx(latest,latest_len);
        assert(!installs && !authorizations);
        begin(mode,"password");rng_fail=1;receive();assert(!installs && !authorizations);
        begin(mode,"password");uint8_t truncated[18]={0};wifi_supplicant_rx(truncated,sizeof(truncated));
        assert(!installs && !authorizations);
    }
    wifi_supplicant_stop();puts("wifi supplicant: vectors, WPA/WPA2 handshake, replays, wrong keys, bad MIC, RNG and validation PASS");
}
