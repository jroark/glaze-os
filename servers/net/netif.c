/*
 * The lwIP network interface over the kernel's selected network driver, address
 * configuration, and the platform hooks lwIP calls (arch/cc.h).
 *
 * lwIP runs in its tcpip thread. The server's main thread hands it the
 * frames the kernel queues (tcpip_input) and takes the core lock to touch
 * the interface.
 *
 * The card can come and go: the main loop asks the kernel about it every
 * CARD_CHECK_MS, attaching the interface to a card inserted after boot and
 * taking it down when the card is pulled.
 *
 * The interface asks DHCP for an address. If nothing answers within
 * DHCP_WAIT_MS it falls back to 10.0.0.1/8 with 10.0.0.254 as router and
 * DNS server, the addresses of the be300 emulator's NAT gateway.
 */

#include <glaze/sys.h>
#include <glaze/wifi.h>
#include <glaze/prefs.h>
#include <glaze/fs.h>
#include <stdio.h>
#include <string.h>

#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "server.h"
#include "wifi_supplicant.h"
#include "utils/includes.h"
#include "utils/common.h"
#include "crypto/sha256.h"

#define DHCP_WAIT_MS    10000u
#define CARD_CHECK_MS   2000u

static struct netif nif;
static struct gz_netif_info info;
static volatile int state = NET_NO_CARD;     /* also set by the tcpip thread */
static uint32_t dhcp_started, card_checked;
static int bound_port;
static bool added;          /* netif_add has run (once, for the first card) */
static bool wireless, associated;
static struct wifi_preference saved;
static int auth_error, auth_failure;
static int connect_network(const struct wifi_credentials *c);
static int save_choice(const struct wifi_preference *choice)
{
    int rv = fs_write_file(PREFS_DIR "/" WIFI_PREF, choice, sizeof(*choice));
    return rv == sizeof(*choice) ? 0 : rv < 0 ? rv : GZ_EIO;
}
static uint8_t frame[1536] __attribute__((aligned(4)));
static uint8_t tx_frame[1536] __attribute__((aligned(4)));
static uint32_t rand_state;

void net_log(const char *fmt, ...)
{
    char line[128];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    gz_log(line, (uint32_t)strlen(line));
}

void net_lwip_assert(const char *msg, const char *file, int line)
{
    net_log("net: lwIP assertion \"%s\" at %s:%d\n", msg, file, line);
    gz_exit(1);
    __builtin_unreachable();
}

uint32_t net_lwip_rand(void)
{
    /* xorshift32, seeded from the clock and the MAC. */
    if (!rand_state)
        rand_state = (uint32_t)gz_time_us() ^ (uint32_t)info.mac[5] << 24 ^ 0x9E3779B9u;
    rand_state ^= rand_state << 13;
    rand_state ^= rand_state >> 17;
    rand_state ^= rand_state << 5;
    return rand_state;
}

static err_t link_output(struct netif *n, struct pbuf *p)
{
    uint16_t len = pbuf_copy_partial(p, tx_frame, sizeof(tx_frame), 0);

    (void)n;
    return gz_net_send(tx_frame, len) < 0 ? ERR_IF : ERR_OK;
}

static err_t nif_init(struct netif *n)
{
    n->name[0] = 'e';
    n->name[1] = 'n';
    n->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(n->hwaddr, info.mac, ETH_HWADDR_LEN);
    n->mtu = 1500;
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    n->output = etharp_output;
    n->linkoutput = link_output;
    netif_set_hostname(n, "be300");
    return ERR_OK;
}

static void status_changed(struct netif *n)
{
    if (ip4_addr_isany_val(*netif_ip4_addr(n)))
        return;
    /* Without a DNS server from DHCP, ask the router. */
    if (ip_addr_isany(dns_getserver(0)))
        dns_setserver(0, netif_ip_gw4(n));
    if (dhcp_supplied_address(n))
        state = NET_UP;
    char a[16], g[16], d[16];
    net_log("net: %s address %s, router %s, dns %s\n", state == NET_UP ? "DHCP" : "static",
            net_ntoa(ip4_addr_get_u32(netif_ip4_addr(n)), a),
            net_ntoa(ip4_addr_get_u32(netif_ip4_gw(n)), g),
            net_ntoa(ip4_addr_get_u32(ip_2_ip4(dns_getserver(0))), d));
}

static void tcpip_ready(void *arg)
{
    sys_sem_signal(arg);
}

/* Brings the interface up on the card the kernel reports in info. */
static int attach(void)
{
    LOCK_TCPIP_CORE();
    if (!added) {
        if (!netif_add(&nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL, nif_init,
                       tcpip_input)) {
            UNLOCK_TCPIP_CORE();
            return GZ_ENOMEM;
        }
        netif_set_status_callback(&nif, status_changed);
        netif_set_default(&nif);
        added = true;
    } else {
        memcpy(nif.hwaddr, info.mac, ETH_HWADDR_LEN);
    }
    wireless = info.kind == GZ_NET_XI825 || info.kind == GZ_NET_WL54CF;
    associated = false;
    netif_set_up(&nif);
    if (!wireless) {
        netif_set_link_up(&nif);
        dhcp_start(&nif);
        dhcp_started = gz_time_ms();
        state = NET_CONFIGURING;
    } else state = NET_DISCONNECTED;
    UNLOCK_TCPIP_CORE();
    gz_net_bind(bound_port);
    if (wireless) {
        uint8_t record[sizeof(saved)+1];
        int len = pref_get(WIFI_PREF, record, sizeof(record));
        memset(&saved, 0, sizeof(saved));
        if (len > 0 && wifi_preference_decode(record, len, &saved) == 0) {
            if ((unsigned)len < sizeof(saved)) save_choice(&saved);
            if (saved.reconnect) {
                int rv = connect_network(&saved.credentials);
                net_log("wifi: saved network association %d\n", rv);
            }
        }
        forced_memzero(record, sizeof(record));
    }
    return 0;
}

/* The card was pulled: no lease to release, just forget the link. */
static void detach(void)
{
    wifi_supplicant_stop();
    auth_error = auth_failure = 0;
    LOCK_TCPIP_CORE();
    dhcp_stop(&nif);
    netif_set_link_down(&nif);
    netif_set_down(&nif);
    netif_set_addr(&nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
    dns_setserver(0, IP_ADDR_ANY);
    associated = false;
    state = NET_NO_CARD;
    UNLOCK_TCPIP_CORE();
    net_log("net: network card removed\n");
}

int net_if_init(int port)
{
    sys_sem_t ready;

    if (sys_sem_new(&ready, 0) != ERR_OK)
        return GZ_ENOMEM;
    tcpip_init(tcpip_ready, &ready);
    sys_arch_sem_wait(&ready, 0);
    sys_sem_free(&ready);
    bound_port = port;
    card_checked = gz_time_ms();
    if (gz_net_info(&info) < 0 || !info.present) {
        net_log("net: no network card\n");
        return GZ_ENETDOWN;
    }
    return attach();
}

static void wifi_link(void)
{
    struct wifi_status status;
    if (!wireless) return;
    if (gz_wifi_ctl(WIFI_STATUS, &status) < 0) return;
    bool up = status.state == WIFI_CONNECTED;
    LOCK_TCPIP_CORE();
    if (up && !associated) {
        netif_set_link_up(&nif);
        state = NET_CONFIGURING;
        dhcp_started = gz_time_ms();
        dhcp_start(&nif);
        net_log("wifi: associated, starting DHCP\n");
    } else if (!up && associated) {
        dhcp_release_and_stop(&nif);
        netif_set_link_down(&nif);
        netif_set_addr(&nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
        dns_setserver(0, IP_ADDR_ANY);
        net_log("wifi: link down\n");
    }
    associated = up;
    if (!up) state = status.state == WIFI_CONNECTING || status.state == WIFI_AUTHENTICATING ? NET_ASSOCIATING : NET_DISCONNECTED;
    UNLOCK_TCPIP_CORE();
}

/* The seed originates from the host CSPRNG in the factory filesystem image.
 * Ratchet it durably before releasing bytes, including across guest reboots. */
static int wifi_random(uint8_t *out, unsigned n)
{
    uint8_t seed[32], next[32], block[32];
    if (pref_get("net.entropy", seed, sizeof(seed)) != sizeof(seed)) return -1;
    uint64_t time = gz_time_us();
    const uint8_t *parts[] = {(const uint8_t *)"wifi nonce", (uint8_t *)&time};
    size_t lengths[] = {10, sizeof(time)};
    if (hmac_sha256_vector(seed, sizeof(seed), 2, parts, lengths, block) < 0) goto fail;
    parts[0] = (const uint8_t *)"wifi ratchet"; lengths[0] = 12;
    if (hmac_sha256_vector(seed, sizeof(seed), 2, parts, lengths, next) < 0 ||
        fs_write_file(PREFS_DIR "/net.entropy", next, sizeof(next)) != sizeof(next)) goto fail;
    if (n > sizeof(block)) goto fail;
    memcpy(out, block, n);
    forced_memzero(seed, sizeof(seed)); forced_memzero(next, sizeof(next)); forced_memzero(block, sizeof(block));
    return 0;
fail:
    forced_memzero(seed, sizeof(seed)); forced_memzero(next, sizeof(next)); forced_memzero(block, sizeof(block));
    return -1;
}
/* DEBUG(hw): the WPA handshake on a real WL54-CF, step by step. */
static int install_key(const struct wifi_key *key)
{
    int rv = gz_wifi_ctl(WIFI_SET_KEY, (void *)key);
    net_log("wifi: install %s key (cipher %u): %d\n", key->pairwise ? "pairwise" : "group",
            key->cipher, rv);
    return rv;
}
static int authorize(void)
{
    int rv = gz_wifi_ctl(WIFI_AUTHORIZE, NULL);
    net_log("wifi: authorize: %d\n", rv);
    return rv;
}
static int send_eapol(const uint8_t *p, unsigned n)
{
    int rv = gz_net_send(p, n);
    net_log("wifi: EAPOL out %u bytes: %d\n", n, rv);
    return rv;
}
static uint32_t wifi_time(void) { return gz_time_ms(); }
static void authentication_failed(int error)
{
    net_log("wifi: authentication failed: %d\n", error);
    auth_failure = error;
    /* Revoke traffic now; defer freeing the state machine until its callback returns. */
    gz_wifi_ctl(WIFI_DISCONNECT, NULL);
}
static const struct wifi_supplicant_io supplicant_io = {
    send_eapol, install_key, authorize, authentication_failed, wifi_random, wifi_time
};
static int connect_network(const struct wifi_credentials *c)
{
    struct wifi_status status;
    int rv = wifi_credentials_validate(c);
    if (rv) return rv;
    rv = gz_wifi_ctl(WIFI_STATUS, &status);
    if (rv) return rv;
    if (!(status.capabilities & WIFI_CAP(c->security))) return WIFI_EUNSUPPORTED;
    /* WL54CF supplies the beacon IE and BSSID needed by the supplicant.
     * PRISM's older scan format allows explicit WEP selection by SSID. */
    rv = gz_wifi_ctl(WIFI_SCAN, NULL); if (rv) return rv;
    rv = gz_wifi_ctl(WIFI_STATUS, &status); if (rv) return rv;
    struct wifi_ap ap = {0};
    for (unsigned i=0; i<status.count; i++)
        if (status.aps[i].ssid_len == c->network.ssid_len &&
            !memcmp(status.aps[i].ssid, c->network.ssid, c->network.ssid_len)) { ap = status.aps[i]; break; }
    rv = gz_wifi_ctl(WIFI_DISCONNECT, NULL); wifi_link();
    if (rv) return rv;
    wifi_supplicant_stop();
    struct wifi_config config;
    rv = wifi_supplicant_prepare(&supplicant_io, c, &ap, info.mac, &config);
    if (rv) return rv;
    auth_error = auth_failure = 0;
    rv = gz_wifi_ctl(WIFI_CONFIGURE, &config);
    forced_memzero(&config, sizeof(config));
    if (rv) wifi_supplicant_stop(); else wifi_supplicant_associated();
    return rv;
}
int net_if_wifi(uint32_t op, void *buf)
{
    if (!wireless) return GZ_ENOENT;
    if (op == WIFI_STATUS) {
        int rv = gz_wifi_ctl(op, buf);
        if (!rv && auth_error) { struct wifi_status *st = buf; st->state = WIFI_FAILED; st->error = auth_error; }
        return rv;
    }
    if (op == WIFI_SCAN) return gz_wifi_ctl(op, buf);
    if (op == WIFI_FORGET) {
        int rv = pref_delete(WIFI_PREF);
        if (rv && rv != GZ_ENOENT) return rv;
        forced_memzero(&saved, sizeof(saved));
        wifi_supplicant_stop(); auth_error = auth_failure = 0;
        rv = gz_wifi_ctl(WIFI_DISCONNECT, NULL); wifi_link(); return rv;
    }
    if (op != WIFI_CONNECT && op != WIFI_CONNECT_SECURE && op != WIFI_DISCONNECT) return GZ_EINVAL;
    struct wifi_preference next = saved;
    if (op != WIFI_DISCONNECT) {
        memset(&next, 0, sizeof(next));
        if (op == WIFI_CONNECT) next.credentials.network = *(struct wifi_request *)buf;
        else { next.credentials = *(struct wifi_credentials *)buf; forced_memzero(buf, sizeof(struct wifi_credentials)); }
        int rv = wifi_credentials_validate(&next.credentials);
        if (rv) { forced_memzero(&next, sizeof(next)); return rv; }
        struct wifi_status status;
        rv = gz_wifi_ctl(WIFI_STATUS, &status);
        if (!rv && !(status.capabilities & WIFI_CAP(next.credentials.security))) rv = WIFI_EUNSUPPORTED;
        if (rv) { forced_memzero(&next, sizeof(next)); return rv; }
    }
    next.version = 2; next.reconnect = op != WIFI_DISCONNECT;
    int rv = save_choice(&next);
    if (!rv) {
        saved = next;
        if (op == WIFI_DISCONNECT) {
            wifi_supplicant_stop(); auth_error = auth_failure = 0;
            rv = gz_wifi_ctl(WIFI_DISCONNECT, NULL);
        } else rv = connect_network(&next.credentials);
    }
    forced_memzero(&next, sizeof(next));
    wifi_link(); return rv;
}

void net_if_poll(void)
{
    int n;
    uint32_t now = gz_time_ms();

    if (now - card_checked >= CARD_CHECK_MS) {
        card_checked = now;
        bool present = gz_net_info(&info) >= 0 && info.present;
        if (state == NET_NO_CARD && present) {
            net_log("net: network card inserted\n");
            attach();
        } else if (state != NET_NO_CARD && !present) {
            detach();
        }
    }
    if (state == NET_NO_CARD)
        return;
    wifi_supplicant_poll();
    if (auth_failure) {
        auth_error = auth_failure; auth_failure = 0;
        gz_wifi_ctl(WIFI_DISCONNECT, NULL); wifi_supplicant_stop();
    }
    wifi_link();
    while ((n = gz_net_recv(frame, sizeof(frame))) > 0) {
        if (wireless && n >= 14 && frame[12] == 0x88 && frame[13] == 0x8e) {
            net_log("wifi: EAPOL in %d bytes\n", n);
            wifi_supplicant_rx(frame, n); continue;
        }
        if (wireless && (!associated || auth_failure)) continue;
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
        if (!p)
            break;              /* out of buffers; later frames stay queued */
        pbuf_take(p, frame, (u16_t)n);
        if (nif.input(p, &nif) != ERR_OK)
            pbuf_free(p);       /* the tcpip thread's mailbox is full */
    }
    if (state == NET_CONFIGURING && gz_time_ms() - dhcp_started > DHCP_WAIT_MS) {
        ip4_addr_t addr, mask, gw;

        LOCK_TCPIP_CORE();
        if (state == NET_CONFIGURING) {     /* DHCP may have just answered */
            net_log("net: no DHCP answer\n");
            dhcp_release_and_stop(&nif);
            IP4_ADDR(&addr, 10, 0, 0, 1);
            IP4_ADDR(&mask, 255, 0, 0, 0);
            IP4_ADDR(&gw, 10, 0, 0, 254);
            state = NET_UP_STATIC;
            netif_set_addr(&nif, &addr, &mask, &gw);
        }
        UNLOCK_TCPIP_CORE();
    }
}

int net_if_state(void)
{
    return state;
}

void net_if_status(struct net_status *st)
{
    memset(st, 0, sizeof(*st));
    st->state = (uint32_t)state;
    gz_net_info(&info);
    memcpy(st->mac, info.mac, sizeof(st->mac));
    st->kind = state == NET_NO_CARD ? GZ_NET_NONE : info.kind;
    st->rx_frames = info.rx_frames;
    st->tx_frames = info.tx_frames;
    st->rx_dropped = info.rx_dropped;
    if (state == NET_UP || state == NET_UP_STATIC) {
        LOCK_TCPIP_CORE();
        st->addr = ip4_addr_get_u32(netif_ip4_addr(&nif));
        st->mask = ip4_addr_get_u32(netif_ip4_netmask(&nif));
        st->gateway = ip4_addr_get_u32(netif_ip4_gw(&nif));
        st->dns = ip4_addr_get_u32(ip_2_ip4(dns_getserver(0)));
        UNLOCK_TCPIP_CORE();
    }
}
