/* Glaze port of the upstream PSK-only RSN supplicant; no socket/EAP daemon. */
#include "wifi_supplicant.h"
#include "utils/includes.h"
#include "utils/common.h"
#include "utils/eloop.h"
#include "crypto/sha1.h"
#include "rsn_supp/wpa.h"
#include "drivers/driver.h"
static struct wpa_sm *sm;
static struct wifi_supplicant_io io;
static enum wpa_states state;
static uint8_t own[6], bssid[6];
static uint32_t started;
static int failure;
static struct {
    bool used; uint32_t at; eloop_timeout_handler handler; void *eloop, *user;
} timers[16];

static void fail(int error)
{
    if (failure) return;
    failure = error;
    if (io.failed) io.failed(error);
}
static void set_state(void *ctx, enum wpa_states value)
{
    (void)ctx;
    state = value;
    if (value == WPA_COMPLETED && io.authorize() < 0) fail(GZ_EIO);
}
static enum wpa_states get_state(void *ctx) { (void)ctx; return state; }
static void deauthenticate(void *ctx, u16 reason) { (void)ctx; (void)reason; fail(WIFI_EAUTH); }
static void reconnect(void *ctx) { (void)ctx; fail(WIFI_EAUTH); }
static int set_key(void *ctx, int link, enum wpa_alg alg, const u8 *addr,
                   int index, int tx, const u8 *seq, size_t seq_len,
                   const u8 *key, size_t key_len, enum key_flag flag)
{
    (void)ctx; (void)link; (void)tx; (void)flag;
    struct wifi_key out = {0};
    if ((alg != WPA_ALG_TKIP && alg != WPA_ALG_CCMP) || index < 0 || index > 3 ||
        seq_len > sizeof(out.sequence) || key_len > sizeof(out.key)) return -1;
    out.cipher = alg == WPA_ALG_TKIP ? WIFI_KEY_TKIP : WIFI_KEY_CCMP;
    out.index = index; out.pairwise = addr && !is_broadcast_ether_addr(addr);
    if (addr) memcpy(out.address, addr, 6);
    out.length = key_len; out.sequence_len = seq_len;
    memcpy(out.key, key, key_len); if (seq_len) memcpy(out.sequence, seq, seq_len);
    int rv = io.key(&out); forced_memzero(&out, sizeof(out));
    return rv < 0 ? -1 : 0;
}
static void *network(void *ctx) { (void)ctx; return &io; }
static int get_bssid(void *ctx, u8 *out) { (void)ctx; memcpy(out, bssid, 6); return 0; }
static int ether_send(void *ctx, const u8 *dest, u16 proto, const u8 *data, size_t n)
{
    (void)ctx;
    uint8_t frame[1514];
    if (n > sizeof(frame)-14 || failure) return -1;
    memcpy(frame, dest, 6); memcpy(frame+6, own, 6);
    frame[12] = proto >> 8; frame[13] = proto;
    memcpy(frame+14, data, n);
    return io.send(frame, n+14) < 0 ? -1 : 0;
}
static int protection(void *ctx, const u8 *addr, int type, int keys)
{ (void)ctx; (void)addr; (void)type; (void)keys; return 0; }
static int beacon(void *ctx) { (void)ctx; return 0; }
static void cancel_timeout(void *ctx) { (void)ctx; }
static u8 *alloc_eapol(void *ctx, u8 type, const void *data, u16 length, size_t *n, void **position)
{
    (void)ctx;
    *n = length + 4;
    u8 *out = os_zalloc(*n); if (!out) return NULL;
    out[0] = 2; out[1] = type; WPA_PUT_BE16(out+2, length);
    if (data) memcpy(out+4, data, length);
    if (position) *position = out+4;
    return out;
}
void wifi_supplicant_stop(void)
{
    if (sm) { wpa_sm_notify_disassoc(sm); wpa_sm_deinit(sm); sm = NULL; }
    memset(timers, 0, sizeof(timers));
    forced_memzero(own, sizeof(own)); forced_memzero(bssid, sizeof(bssid));
    state = WPA_DISCONNECTED; failure = 0;
}
int wifi_supplicant_prepare(const struct wifi_supplicant_io *callbacks,
                            const struct wifi_credentials *c, const struct wifi_ap *ap,
                            const uint8_t mac[6], struct wifi_config *config)
{
    int rv = wifi_credentials_validate(c); if (rv) return rv;
    wifi_supplicant_stop(); io = *callbacks;
    memset(config, 0, sizeof(*config)); config->network = c->network;
    config->security = c->security; config->key_index = c->key_index; config->shared_auth = c->shared_auth;
    memcpy(config->bssid, ap->bssid, 6);
    if (c->security < WIFI_WPA_PSK) {
        if (c->security == WIFI_WEP) config->key_len = wifi_wep_decode(c, config->key);
        return 0;
    }
    if (!ap->ie_len || ap->ie_len > sizeof(ap->ie)) return WIFI_EUNSUPPORTED;
    struct wpa_ie_data parsed;
    if (wpa_parse_wpa_ie(ap->ie, ap->ie_len, &parsed) < 0 ||
        !(parsed.key_mgmt & WPA_KEY_MGMT_PSK) ||
        !(parsed.proto & (c->security == WIFI_WPA_PSK ? WPA_PROTO_WPA : WPA_PROTO_RSN))) return WIFI_EUNSUPPORTED;
    int pair = c->security == WIFI_WPA_PSK ? WPA_CIPHER_TKIP : WPA_CIPHER_CCMP;
    if (!(parsed.pairwise_cipher & pair) ||
        (parsed.group_cipher != WPA_CIPHER_TKIP && parsed.group_cipher != WPA_CIPHER_CCMP) ||
        (parsed.capabilities & WPA_CAPABILITY_MFPR)) return WIFI_EUNSUPPORTED;
    uint8_t pmk[32];
    if (c->key_len == 64) {
        char hex[65]; memcpy(hex, c->key, 64); hex[64] = 0;
        rv = hexstr2bin(hex, pmk, sizeof(pmk)); forced_memzero(hex, sizeof(hex));
    } else {
        char pass[64]; memcpy(pass, c->key, c->key_len); pass[c->key_len] = 0;
        rv = pbkdf2_sha1(pass, c->network.ssid, c->network.ssid_len, 4096, pmk, sizeof(pmk));
        forced_memzero(pass, sizeof(pass));
    }
    if (rv < 0) { forced_memzero(pmk, sizeof(pmk)); return GZ_EIO; }
    struct wpa_sm_ctx *ctx = os_zalloc(sizeof(*ctx));
    if (!ctx) { forced_memzero(pmk, sizeof(pmk)); return GZ_ENOMEM; }
    ctx->set_state = set_state; ctx->get_state = get_state; ctx->deauthenticate = deauthenticate;
    ctx->reconnect = reconnect; ctx->set_key = set_key; ctx->get_network_ctx = network;
    ctx->get_bssid = get_bssid; ctx->ether_send = ether_send; ctx->get_beacon_ie = beacon;
    ctx->cancel_auth_timeout = cancel_timeout; ctx->alloc_eapol = alloc_eapol;
    ctx->mlme_setprotection = protection;
    sm = wpa_sm_init(ctx);
    if (!sm) { os_free(ctx); forced_memzero(pmk, sizeof(pmk)); return GZ_ENOMEM; }
    memcpy(own, mac, 6); memcpy(bssid, ap->bssid, 6);
    wpa_sm_set_own_addr(sm, own); wpa_sm_set_ssid(sm, c->network.ssid, c->network.ssid_len);
    wpa_sm_set_param(sm, WPA_PARAM_PROTO, c->security == WIFI_WPA_PSK ? WPA_PROTO_WPA : WPA_PROTO_RSN);
    wpa_sm_set_param(sm, WPA_PARAM_KEY_MGMT, WPA_KEY_MGMT_PSK);
    wpa_sm_set_param(sm, WPA_PARAM_PAIRWISE, pair);
    wpa_sm_set_param(sm, WPA_PARAM_GROUP, parsed.group_cipher);
    wpa_sm_set_param(sm, WPA_PARAM_RSN_ENABLED, c->security == WIFI_WPA2_PSK);
    wpa_sm_set_pmk(sm, pmk, sizeof(pmk), NULL, NULL); forced_memzero(pmk, sizeof(pmk));
    if (c->security == WIFI_WPA_PSK) rv = wpa_sm_set_ap_wpa_ie(sm, ap->ie, ap->ie_len);
    else rv = wpa_sm_set_ap_rsn_ie(sm, ap->ie, ap->ie_len);
    size_t n = sizeof(config->ie);
    if (rv < 0 || wpa_sm_set_assoc_wpa_ie_default(sm, config->ie, &n) < 0) {
        wifi_supplicant_stop(); return WIFI_EUNSUPPORTED;
    }
    config->ie_len = n; return 0;
}
void wifi_supplicant_associated(void)
{
    if (sm) { started = io.time_ms(); state = WPA_ASSOCIATED; wpa_sm_notify_assoc(sm, bssid); }
}
void wifi_supplicant_rx(const uint8_t *frame, unsigned n)
{
    if (!sm || failure || n < 18 || memcmp(frame, own, 6) || memcmp(frame+6, bssid, 6) ||
        frame[12] != 0x88 || frame[13] != 0x8e) return;
    wpa_sm_rx_eapol(sm, frame+6, frame+14, n-14, FRAME_ENCRYPTION_UNKNOWN);
}
void wifi_supplicant_poll(void)
{
    if (!sm || failure) return;
    uint32_t now = io.time_ms();
    if (state != WPA_COMPLETED && now - started >= 15000) { fail(WIFI_EAUTH); return; }
    for (unsigned i = 0; i < ARRAY_SIZE(timers); i++) {
        if (timers[i].used && (int32_t)(now - timers[i].at) >= 0) {
            eloop_timeout_handler h = timers[i].handler;
            void *e = timers[i].eloop, *u = timers[i].user; timers[i].used = false;
            h(e, u);
        }
    }
}
int wifi_supplicant_state(void) { return !sm ? WIFI_DISCONNECTED : failure ? failure : state == WPA_COMPLETED ? WIFI_CONNECTED : WIFI_AUTHENTICATING; }
int os_get_random(unsigned char *out, size_t n)
{ int rv = io.random && n <= 4096 ? io.random(out, n) : -1; if (rv) fail(WIFI_ERANDOM); return rv; }
int os_get_reltime(struct os_reltime *t) { uint32_t n = io.time_ms(); t->sec = n/1000; t->usec = (n%1000)*1000; return 0; }
void *os_zalloc(size_t n) { void *p = malloc(n); if (p) memset(p, 0, n); return p; }
void *os_memdup(const void *p, size_t n) { void *out = malloc(n); if (out) memcpy(out, p, n); return out; }
int eloop_register_timeout(unsigned sec, unsigned usec, eloop_timeout_handler h, void *e, void *u)
{
    for (unsigned i=0; i<ARRAY_SIZE(timers); i++) if (!timers[i].used) {
        timers[i].used = true; timers[i].at = io.time_ms() + sec*1000 + usec/1000;
        timers[i].handler = h; timers[i].eloop = e; timers[i].user = u; return 0;
    }
    fail(GZ_ENOMEM); return -1;
}
int eloop_cancel_timeout(eloop_timeout_handler h, void *e, void *u)
{
    int n=0;
    for (unsigned i=0; i<ARRAY_SIZE(timers); i++)
        if (timers[i].used && timers[i].handler == h &&
            (e == ELOOP_ALL_CTX || e == timers[i].eloop) && (u == ELOOP_ALL_CTX || u == timers[i].user))
            { timers[i].used = false; n++; }
    return n;
}

int os_get_time(struct os_time *t) { uint32_t n=io.time_ms(); t->sec=n/1000; t->usec=(n%1000)*1000; return 0; }
int os_memcmp_const(const void *a, const void *b, size_t n)
{
    const uint8_t *x=a, *y=b; uint8_t v=0;
    for (size_t i=0;i<n;i++) v |= x[i]^y[i];
    return v;
}
