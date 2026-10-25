/* Shared validation, before any preference or device mutation. */
#include <glaze/wifi.h>
#include <string.h>
static int hex(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
int wifi_credentials_validate(const struct wifi_credentials *c)
{
    if (!c || !c->network.ssid_len || c->network.ssid_len > WIFI_SSID_MAX ||
        c->security > WIFI_WPA2_PSK || c->key_len > 64 || c->key_index > 3 || c->shared_auth > 1)
        return GZ_EINVAL;
    unsigned n = c->key_len;
    if (c->security == WIFI_OPEN) return n || c->key_index || c->shared_auth ? GZ_EINVAL : 0;
    if (c->security == WIFI_WEP) {
        if (n != 5 && n != 13 && n != 10 && n != 26) return GZ_EINVAL;
    } else if (n < 8 || c->key_index || c->shared_auth) return GZ_EINVAL;
    bool hexadecimal = c->security == WIFI_WEP ? n == 10 || n == 26 : n == 64;
    for (unsigned i = 0; i < n; i++) {
        if (hexadecimal ? hex(c->key[i]) < 0 : c->key[i] < 32 || c->key[i] > 126) return GZ_EINVAL;
    }
    return 0;
}
int wifi_wep_decode(const struct wifi_credentials *c, uint8_t key[13])
{
    if (wifi_credentials_validate(c) || c->security != WIFI_WEP) return GZ_EINVAL;
    bool hexadecimal = c->key_len == 10 || c->key_len == 26;
    unsigned n = hexadecimal ? c->key_len / 2 : c->key_len;
    for (unsigned i = 0; i < n; i++) key[i] = hexadecimal ? hex(c->key[2*i]) * 16 + hex(c->key[2*i+1]) : c->key[i];
    return n;
}

int wifi_preference_decode(const void *record, unsigned n, struct wifi_preference *out)
{
    struct { uint32_t version, reconnect; struct wifi_request network; } legacy;
    memset(out, 0, sizeof(*out));
    if (!record) return GZ_EINVAL;
    if (n == sizeof(legacy)) {
        memcpy(&legacy, record, n);
        if (legacy.version != 1 || legacy.reconnect > 1) return GZ_EINVAL;
        out->version = 2; out->reconnect = legacy.reconnect;
        out->credentials.network = legacy.network;
    } else if (n == sizeof(*out)) memcpy(out, record, n);
    else return GZ_EINVAL;
    if (out->version != 2 || out->reconnect > 1 || wifi_credentials_validate(&out->credentials)) {
        memset(out, 0, sizeof(*out)); return GZ_EINVAL;
    }
    return 0;
}
