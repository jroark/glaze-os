/* Infrastructure Wi-Fi control. SSIDs are bytes with explicit lengths. */
#ifndef GLAZE_WIFI_H
#define GLAZE_WIFI_H
#include <glaze/abi.h>
#define WIFI_SSID_MAX 32
#define WIFI_AP_MAX 16
#define WIFI_PREF "net.wifi"
#define WIFI_IE_MAX 80
enum { WIFI_STATUS, WIFI_SCAN, WIFI_CONNECT, WIFI_DISCONNECT,
       WIFI_CONNECT_SECURE, WIFI_FORGET,
       /* Private PROC_NET driver operations. */
       WIFI_CONFIGURE, WIFI_SET_KEY, WIFI_AUTHORIZE };
enum { WIFI_ABSENT, WIFI_DISCONNECTED, WIFI_CONNECTING, WIFI_CONNECTED, WIFI_FAILED, WIFI_AUTHENTICATING };
struct wifi_ap {
    uint8_t ssid[WIFI_SSID_MAX];
    uint8_t ssid_len, bssid[6], channel;
    uint16_t signal, noise, rate; /* firmware levels; rate in half Mbps */
    uint16_t privacy;            /* capability privacy bit; security may be unknown */
    uint16_t ie_len;
    uint8_t ie[WIFI_IE_MAX];
};
enum { WIFI_OPEN, WIFI_WEP, WIFI_WPA_PSK, WIFI_WPA2_PSK };
#define WIFI_CAP(mode) (1u << (mode))
#define WIFI_EUNSUPPORTED (-23)
#define WIFI_EAUTH (-24)
#define WIFI_ERANDOM (-25)
struct wifi_status {
    uint32_t state, scanning;
    int32_t error;
    uint32_t count;
    struct wifi_ap current, aps[WIFI_AP_MAX];
    uint32_t capabilities, security;
};
struct wifi_request {
    uint32_t ssid_len;
    uint8_t ssid[WIFI_SSID_MAX];
};
struct wifi_credentials {
    struct wifi_request network;
    uint32_t security, key_len, key_index, shared_auth;
    uint8_t key[64]; /* WEP ASCII/hex, WPA passphrase/hex PSK */
};
/* Kernel receives only association settings and keys, never a passphrase. */
struct wifi_config {
    struct wifi_request network;
    uint32_t security, key_len, key_index, shared_auth, ie_len;
    uint8_t key[13], bssid[6], ie[WIFI_IE_MAX];
};
enum { WIFI_KEY_NONE, WIFI_KEY_TKIP, WIFI_KEY_CCMP };
struct wifi_key {
    uint32_t cipher, index, pairwise, length, sequence_len;
    uint8_t address[6], sequence[8], key[32];
};
int wifi_credentials_validate(const struct wifi_credentials *c);
int wifi_wep_decode(const struct wifi_credentials *c, uint8_t key[13]);
/* Versioned persistent choice, used only by the network server. */
struct wifi_preference {
    uint32_t version, reconnect;
    struct wifi_credentials credentials;
};
int wifi_preference_decode(const void *record, unsigned length, struct wifi_preference *out);
int net_wifi_status(struct wifi_status *status);
int net_wifi_scan(void);
int net_wifi_connect(const void *ssid, uint32_t length);
int net_wifi_disconnect(void);
int net_wifi_connect_secure(const struct wifi_credentials *credentials);
int net_wifi_forget(void);
#endif
