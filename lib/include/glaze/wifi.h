/* Open infrastructure Wi-Fi control. SSIDs are bytes with explicit lengths. */
#ifndef GLAZE_WIFI_H
#define GLAZE_WIFI_H
#include <glaze/abi.h>
#define WIFI_SSID_MAX 32
#define WIFI_AP_MAX 16
#define WIFI_PREF "net.wifi"
enum { WIFI_STATUS, WIFI_SCAN, WIFI_CONNECT, WIFI_DISCONNECT };
enum { WIFI_ABSENT, WIFI_DISCONNECTED, WIFI_CONNECTING, WIFI_CONNECTED, WIFI_FAILED };
struct wifi_ap {
    uint8_t ssid[WIFI_SSID_MAX];
    uint8_t ssid_len, bssid[6], channel;
    uint16_t signal, noise, rate; /* firmware levels; rate in half Mbps */
    uint16_t privacy;            /* nonzero: security unsupported */
};
struct wifi_status {
    uint32_t state, scanning;
    int32_t error;
    uint32_t count;
    struct wifi_ap current, aps[WIFI_AP_MAX];
};
struct wifi_request {
    uint32_t ssid_len;
    uint8_t ssid[WIFI_SSID_MAX];
};
/* Versioned persistent choice, used only by the network server. */
struct wifi_preference {
    uint32_t version, reconnect;
    struct wifi_request network;
};
int net_wifi_status(struct wifi_status *status);
int net_wifi_scan(void);
int net_wifi_connect(const void *ssid, uint32_t length);
int net_wifi_disconnect(void);
#endif
