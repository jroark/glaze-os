/*
 * The lwIP network interface over the kernel's selected network driver, address
 * configuration, and the platform hooks lwIP calls (arch/cc.h).
 *
 * lwIP runs in its tcpip thread. The server's main thread hands it the
 * frames the kernel queues (tcpip_input) and takes the core lock to touch
 * the interface.
 *
 * The interface asks DHCP for an address. If nothing answers within
 * DHCP_WAIT_MS it falls back to 10.0.0.1/8 with 10.0.0.254 as router and
 * DNS server, the addresses of the be300 emulator's NAT gateway.
 */

#include <glaze/sys.h>
#include <glaze/wifi.h>
#include <glaze/prefs.h>
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

#define DHCP_WAIT_MS    10000u

static struct netif nif;
static struct gz_netif_info info;
static volatile int state = NET_NO_CARD;     /* also set by the tcpip thread */
static uint32_t dhcp_started;
static bool wireless, associated;
static struct wifi_preference saved;
static uint8_t frame[1536] __attribute__((aligned(4)));
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
    uint16_t len = pbuf_copy_partial(p, frame, sizeof(frame), 0);

    (void)n;
    return gz_net_send(frame, len) < 0 ? ERR_IF : ERR_OK;
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

int net_if_init(int port)
{
    sys_sem_t ready;

    if (sys_sem_new(&ready, 0) != ERR_OK)
        return GZ_ENOMEM;
    tcpip_init(tcpip_ready, &ready);
    sys_arch_sem_wait(&ready, 0);
    sys_sem_free(&ready);
    if (gz_net_info(&info) < 0 || !info.present) {
        net_log("net: no network card\n");
        return GZ_ENETDOWN;
    }
    LOCK_TCPIP_CORE();
    struct netif *n = netif_add(&nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL, nif_init, tcpip_input);
    if (n) {
        netif_set_status_callback(&nif, status_changed);
        netif_set_default(&nif);
        wireless = info.kind == GZ_NET_XI825;
        netif_set_up(&nif);
        if (!wireless) {
            netif_set_link_up(&nif);
            dhcp_start(&nif);
            dhcp_started = gz_time_ms();
            state = NET_CONFIGURING;
        } else state = NET_DISCONNECTED;
    }
    UNLOCK_TCPIP_CORE();
    if (!n)
        return GZ_ENOMEM;
    gz_net_bind(port);
    if (wireless) {
        int len = pref_get(WIFI_PREF, &saved, sizeof(saved));
        if (len == sizeof(saved) && saved.version == 1 && saved.reconnect == 1 &&
            saved.network.ssid_len > 0 && saved.network.ssid_len <= WIFI_SSID_MAX) {
            int rv = gz_wifi_ctl(WIFI_CONNECT, &saved.network);
            net_log("wifi: saved network association %d\n", rv);
        } else memset(&saved, 0, sizeof(saved));
    }
    return 0;
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
    if (!up) state = status.state == WIFI_CONNECTING ? NET_ASSOCIATING : NET_DISCONNECTED;
    UNLOCK_TCPIP_CORE();
}

int net_if_wifi(uint32_t op, void *buf)
{
    if (!wireless) return GZ_ENOENT;
    if (op == WIFI_STATUS || op == WIFI_SCAN) return gz_wifi_ctl(op, buf);
    if (op != WIFI_CONNECT && op != WIFI_DISCONNECT) return GZ_EINVAL;
    struct wifi_preference next = saved;
    if (op == WIFI_CONNECT) {
        struct wifi_request *req = buf;
        if (!req->ssid_len || req->ssid_len > WIFI_SSID_MAX) return GZ_EINVAL;
        memset(&next, 0, sizeof(next));
        next.network = *req;
    }
    next.version = 1;
    next.reconnect = op == WIFI_CONNECT;
    /* Persist the intent before changing the card: errors remain reviewable. */
    int rv = pref_put(WIFI_PREF, &next, sizeof(next));
    if (rv) return rv;
    saved = next;
    /* Force the old link down before another association can complete, so
     * switching networks always discards the previous lease and DNS. */
    if (op == WIFI_CONNECT) {
        rv = gz_wifi_ctl(WIFI_DISCONNECT, NULL);
        wifi_link();
        if (rv) return rv;
    }
    rv = gz_wifi_ctl(op, buf);
    wifi_link();
    return rv;
}

void net_if_poll(void)
{
    int n;

    if (state == NET_NO_CARD)
        return;
    wifi_link();
    while ((n = gz_net_recv(frame, sizeof(frame))) > 0) {
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
