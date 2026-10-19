/*
 * The lwIP network interface over the kernel's NE2000 driver, address
 * configuration, and the platform hooks lwIP calls (arch/cc.h).
 *
 * The interface asks DHCP for an address. If nothing answers within
 * DHCP_WAIT_MS it falls back to 10.0.0.1/8 with 10.0.0.254 as router and
 * DNS server, the addresses of the be300 emulator's NAT gateway.
 */

#include <glaze/sys.h>
#include <stdio.h>
#include <string.h>

#include "lwip/init.h"
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
static int state = NET_NO_CARD;
static uint32_t dhcp_started;
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

u32_t sys_now(void)
{
    return gz_time_ms();
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

int net_if_init(int port)
{
    lwip_init();
    if (gz_net_info(&info) < 0 || !info.present) {
        net_log("net: no network card\n");
        return GZ_ENETDOWN;
    }
    if (!netif_add(&nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL, nif_init, ethernet_input))
        return GZ_ENOMEM;
    netif_set_status_callback(&nif, status_changed);
    netif_set_default(&nif);
    netif_set_link_up(&nif);
    netif_set_up(&nif);
    gz_net_bind(port);
    dhcp_start(&nif);
    dhcp_started = gz_time_ms();
    state = NET_CONFIGURING;
    return 0;
}

void net_if_poll(void)
{
    int n;

    if (state == NET_NO_CARD)
        return;
    while ((n = gz_net_recv(frame, sizeof(frame))) > 0) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
        if (!p)
            break;              /* out of buffers; later frames stay queued */
        pbuf_take(p, frame, (u16_t)n);
        if (nif.input(p, &nif) != ERR_OK)
            pbuf_free(p);
    }
    /* Each pass over the loopback queue can queue replies. */
    for (int i = 0; i < 16 && nif.loop_first; i++)
        netif_poll(&nif);
    if (state == NET_CONFIGURING && gz_time_ms() - dhcp_started > DHCP_WAIT_MS) {
        ip4_addr_t addr, mask, gw;

        net_log("net: no DHCP answer\n");
        dhcp_release_and_stop(&nif);
        IP4_ADDR(&addr, 10, 0, 0, 1);
        IP4_ADDR(&mask, 255, 0, 0, 0);
        IP4_ADDR(&gw, 10, 0, 0, 254);
        state = NET_UP_STATIC;
        netif_set_addr(&nif, &addr, &mask, &gw);
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
        st->addr = ip4_addr_get_u32(netif_ip4_addr(&nif));
        st->mask = ip4_addr_get_u32(netif_ip4_netmask(&nif));
        st->gateway = ip4_addr_get_u32(netif_ip4_gw(&nif));
        st->dns = ip4_addr_get_u32(ip_2_ip4(dns_getserver(0)));
    }
}
