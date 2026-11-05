/*
 * Dial-up over a CF modem: the Hayes dialer (hayes.c) and lwIP's PPP over
 * serial on the kernel's modem UART (gz_modem_io, kernel/modem.c).
 *
 * The main thread reads the modem when the kernel says bytes are waiting
 * (GZ_NOTIFY_NET) and at least once a second: while dialing they go to the
 * dialer, once connected to pppos_input_tcpip. lwIP's tcpip thread writes
 * PPP frames back through output(). The modem drops the call when DTR goes
 * low (AT&D2); DCD, once it has been seen up, reports a lost carrier.
 */
#include <glaze/sys.h>
#include <glaze/modem.h>
#include <glaze/prefs.h>
#include <string.h>
#include "server.h"

#if !GLAZE_MODEM
/* A build without PPP (MODEM=0, mk/libs.mk): the modem is reported, and
 * dialing explains itself, so Settings can say why. */
static bool modem_ok;
int net_modem_init(int port)
{
    (void)port;
    if (gz_modem_io(MODEM_IO_STATUS, NULL, 0) < 0) return GZ_ENOENT;
    modem_ok = true;
    net_log("modem: present, but this build has no PPP (MODEM=1)\n");
    return 0;
}
bool net_modem_present(void) { return modem_ok; }
void net_modem_poll(void) {}
int net_modem_request(uint32_t op, void *buf)
{
    if (op == NET_DIAL_STATUS) {
        struct net_dial_status *st = buf;
        memset(st, 0, sizeof(*st));
        st->error = NET_DIAL_E_UNSUPPORTED;
        return 0;
    }
    if (op == NET_DIAL_CONFIG) { memset(buf, 0, sizeof(struct net_dial_config)); return 0; }
    if (op == NET_DIAL) memset(buf, 0, sizeof(struct net_dial_config));
    return op == NET_HANGUP ? 0 : GZ_ENOENT;
}
int net_modem_state(void) { return NET_DISCONNECTED; }
void net_modem_status(struct net_status *st)
{
    memset(st, 0, sizeof(*st));
    st->state = NET_DISCONNECTED;
    st->kind = GZ_NET_MODEM;
}
#else

#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/dns.h"
#include "netif/ppp/pppos.h"
#include "hayes.h"

#define DIAL_PREF       "net.dialup"
#define HANGUP_MS       1500u   /* DTR low: longer than the modem's S25 (50 ms) */
#define CLOSE_MS        3000u   /* LCP terminate before dropping DTR anyway */
#define WRITE_WAIT_MS   2000u

static ppp_pcb *pcb;
static struct netif pppif;
static struct hayes dialer;
static struct net_dial_config saved;     /* ppp_set_auth keeps pointers into it */
static volatile int dial_state = NET_DIAL_IDLE;
static volatile int dial_error, ppp_ended;
static uint32_t hangup_at, dtr_at, up_since, rx_bytes;
static bool dtr_dropped, dtr_raised;
static volatile uint32_t tx_bytes;
static bool carrier_seen, modem_ok;
static char info[32];

/* The dialer's commands and replies, without the CRs. Dial strings carry
 * no credentials (PAP/CHAP run inside PPP). */
static void trace(char direction, const char *text)
{
    char line[96];
    unsigned n = 0;
    for (const char *p = text; *p && n + 1 < sizeof(line); p++)
        if (*p != '\r') line[n++] = *p;
    line[n] = 0;
    if (n) net_log("modem: %c %s\n", direction, line);
}

static int modem_write(const void *p, uint32_t n)
{
    const uint8_t *d = p;
    uint32_t done = 0, t0 = gz_time_ms();
    while (done < n) {
        uint32_t chunk = n - done < MODEM_IO_MAX ? n - done : MODEM_IO_MAX;
        int rv = gz_modem_io(MODEM_IO_WRITE, (void *)(d + done), chunk);
        if (rv == GZ_EBUSY && gz_time_ms() - t0 < WRITE_WAIT_MS) { gz_sleep_ms(5); continue; }
        if (rv <= 0) break;
        done += (uint32_t)rv;
    }
    return (int)done;
}

/* tcpip thread: a PPP frame, HDLC-escaped by pppos. */
static u32_t output(ppp_pcb *p, const void *data, u32_t len, void *ctx)
{
    (void)p; (void)ctx;
    int n = modem_write(data, len);
    tx_bytes += (uint32_t)n;
    return (u32_t)n;
}

/* tcpip thread: the PPP link's progress. */
static void link_status(ppp_pcb *p, int err, void *ctx)
{
    (void)p; (void)ctx;
    if (err == PPPERR_NONE) {
        char a[16], g[16], d[16];
        up_since = gz_time_ms();
        dial_state = NET_DIAL_UP;
        net_log("modem: PPP up, address %s, peer %s, dns %s\n",
                net_ntoa(ip4_addr_get_u32(netif_ip4_addr(&pppif)), a),
                net_ntoa(ip4_addr_get_u32(netif_ip4_gw(&pppif)), g),
                net_ntoa(ip4_addr_get_u32(ip_2_ip4(dns_getserver(0))), d));
        return;
    }
    if (!dial_error && err != PPPERR_USER)
        dial_error = err == PPPERR_AUTHFAIL ? NET_DIAL_E_AUTH
                   : err == PPPERR_CONNECT ? NET_DIAL_E_NO_CARRIER : NET_DIAL_E_PPP;
    net_log("modem: PPP ended (%d)\n", err);
    ppp_ended = 1;
}

/* Starts dropping the call: PPP first if it's running, then DTR. */
static void hang_up(int error)
{
    if (dial_state == NET_DIAL_IDLE || dial_state == NET_DIAL_HANGING_UP) return;
    if (error && !dial_error) dial_error = error;
    bool ppp = dial_state == NET_DIAL_NEGOTIATING || dial_state == NET_DIAL_UP;
    dial_state = NET_DIAL_HANGING_UP;
    hangup_at = gz_time_ms();
    dtr_dropped = dtr_raised = false;
    if (ppp && !ppp_ended) {
        LOCK_TCPIP_CORE();
        /* Without a carrier there is no one to send LCP Terminate to. */
        ppp_close(pcb, error == NET_DIAL_E_NO_CARRIER);
        UNLOCK_TCPIP_CORE();
    } else ppp_ended = 1;
}

static void start_ppp(void)
{
    LOCK_TCPIP_CORE();
    ppp_set_auth(pcb, saved.user[0] ? PPPAUTHTYPE_ANY : PPPAUTHTYPE_NONE, saved.user, saved.password);
    ppp_set_usepeerdns(pcb, 1);
    ppp_set_default(pcb);
    ppp_connect(pcb, 0);
    UNLOCK_TCPIP_CORE();
    ppp_ended = 0;
    carrier_seen = false;
}

int net_modem_init(int port)
{
    int msr = gz_modem_io(MODEM_IO_STATUS, NULL, 0);
    if (msr < 0) return msr;
    LOCK_TCPIP_CORE();
    pcb = pppos_create(&pppif, output, link_status, NULL);
    if (pcb) netif_set_hostname(&pppif, "be300");
    UNLOCK_TCPIP_CORE();
    if (!pcb) return GZ_ENOMEM;
    uint8_t record[sizeof(saved) + 1];
    int len = pref_get(DIAL_PREF, record, sizeof(record));
    if (len == (int)sizeof(saved)) memcpy(&saved, record, sizeof(saved));
    if (net_dial_config_validate(&saved)) memset(&saved, 0, sizeof(saved));
    memset(record, 0, sizeof(record));
    gz_modem_io(MODEM_IO_BIND, NULL, (uint32_t)port);
    gz_modem_io(MODEM_IO_LINES, NULL, MODEM_DTR | MODEM_RTS);
    dialer.trace = trace;
    modem_ok = true;
    net_log("modem: ready, dialing on request (DCD %s)\n", msr & MODEM_MSR_DCD ? "up" : "down");
    return 0;
}

bool net_modem_present(void) { return modem_ok; }

void net_modem_poll(void)
{
    if (!modem_ok) return;
    uint32_t now = gz_time_ms();
    uint8_t buf[MODEM_IO_MAX];
    int n;
    while ((n = gz_modem_io(MODEM_IO_READ, buf, sizeof(buf))) > 0) {
        unsigned used = 0;
        if (dial_state == NET_DIAL_STARTING || dial_state == NET_DIAL_DIALING) {
            used = hayes_input(&dialer, buf, (unsigned)n, now);
            /* A failed attempt is handled below, with the timeouts. */
            if (dialer.state == NET_DIAL_NEGOTIATING) {
                dial_state = NET_DIAL_NEGOTIATING;
                strlcpy(info, dialer.info, sizeof(info));
                net_log("modem: CONNECT %u\n", (unsigned)dialer.rate);
                start_ppp();
            }
        }
        /* While hanging up too: the peer's Terminate-Ack ends PPP. */
        bool ppp_running = dial_state == NET_DIAL_NEGOTIATING || dial_state == NET_DIAL_UP ||
                           (dial_state == NET_DIAL_HANGING_UP && !ppp_ended);
        if (used < (unsigned)n && ppp_running) {
            rx_bytes += (unsigned)n - used;
            pppos_input_tcpip(pcb, buf + used, n - (int)used);
        }
    }
    if (n == GZ_EDEAD || n == GZ_ENOENT) {
        /* The card was pulled; the kernel restarts the server for the next one. */
        modem_ok = false;
        dial_state = NET_DIAL_IDLE;
        dial_error = NET_DIAL_E_NO_CARRIER;
        return;
    }
    if (dial_state == NET_DIAL_STARTING || dial_state == NET_DIAL_DIALING) {
        const char *cmd = hayes_command(&dialer);
        if (cmd) modem_write(cmd, strlen(cmd));
        hayes_tick(&dialer, now);
        if (dialer.state == NET_DIAL_IDLE) {
            net_log("modem: dialing failed (%d)\n", dialer.error);
            dial_state = NET_DIAL_DIALING;   /* hang_up's guard: a call may be half made */
            hang_up(dialer.error);
        } else dial_state = dialer.state;
    }
    if (dial_state == NET_DIAL_NEGOTIATING || dial_state == NET_DIAL_UP) {
        int msr = gz_modem_io(MODEM_IO_STATUS, NULL, 0);
        if (msr >= 0 && (msr & MODEM_MSR_DCD)) carrier_seen = true;
        else if (msr >= 0 && carrier_seen) {
            net_log("modem: carrier lost\n");
            hang_up(NET_DIAL_E_NO_CARRIER);
        }
        if (ppp_ended && dial_state != NET_DIAL_HANGING_UP) hang_up(0);
    }
    if (dial_state == NET_DIAL_HANGING_UP) {
        /* Drop DTR once PPP has said goodbye, or anyway after CLOSE_MS;
         * PPP itself may take its Terminate-Request retries to finish, and
         * the next call waits for it (lwIP needs the session dead). */
        if (!dtr_dropped && (ppp_ended || now - hangup_at >= CLOSE_MS)) {
            gz_modem_io(MODEM_IO_LINES, NULL, MODEM_RTS);
            dtr_dropped = true;
            dtr_at = now;
        } else if (dtr_dropped && !dtr_raised && now - dtr_at >= HANGUP_MS) {
            gz_modem_io(MODEM_IO_LINES, NULL, MODEM_DTR | MODEM_RTS);
            dtr_raised = true;
        }
        if (dtr_raised && ppp_ended) {
            dial_state = NET_DIAL_IDLE;
            net_log("modem: hung up\n");
        }
    }
}

int net_modem_request(uint32_t op, void *buf)
{
    if (!modem_ok) return GZ_ENOENT;
    switch (op) {
    case NET_DIAL: {
        struct net_dial_config next = *(struct net_dial_config *)buf;
        memset(buf, 0, sizeof(next));
        int rv = net_dial_config_validate(&next);
        if (!rv && dial_state != NET_DIAL_IDLE) rv = GZ_EBUSY;
        if (!rv && !next.number[0]) {
            if (!saved.number[0]) rv = GZ_EINVAL;
            else next = saved;
        } else if (!rv && next.keep_password) {
            memcpy(next.password, saved.password, sizeof(next.password));
        }
        if (rv) { memset(&next, 0, sizeof(next)); return rv; }
        next.has_password = next.keep_password = 0;
        rv = pref_put(DIAL_PREF, &next, sizeof(next));
        if (rv < 0) net_log("modem: settings not saved (%d)\n", rv);
        saved = next;
        memset(&next, 0, sizeof(next));
        dial_error = NET_DIAL_E_NONE;
        hayes_start(&dialer, &saved, gz_time_ms());
        dial_state = dialer.state;
        net_log("modem: dialing\n");
        return 0;
    }
    case NET_HANGUP:
        hang_up(0);
        return 0;
    case NET_DIAL_STATUS: {
        struct net_dial_status *st = buf;
        memset(st, 0, sizeof(*st));
        st->state = (uint32_t)dial_state;
        st->error = dial_error;
        st->rate = dialer.rate;
        st->rx_bytes = rx_bytes;
        st->tx_bytes = tx_bytes;
        strlcpy(st->modem, info[0] ? info : dialer.info, sizeof(st->modem));
        if (dial_state == NET_DIAL_UP) {
            st->online_ms = gz_time_ms() - up_since;
            LOCK_TCPIP_CORE();
            st->addr = ip4_addr_get_u32(netif_ip4_addr(&pppif));
            st->peer = ip4_addr_get_u32(netif_ip4_gw(&pppif));
            st->dns = ip4_addr_get_u32(ip_2_ip4(dns_getserver(0)));
            UNLOCK_TCPIP_CORE();
        }
        return 0;
    }
    case NET_DIAL_CONFIG: {
        struct net_dial_config *c = buf;
        *c = saved;
        memset(c->password, 0, sizeof(c->password));
        c->has_password = saved.password[0] != 0;
        return 0;
    }
    }
    return GZ_EINVAL;
}

int net_modem_state(void)
{
    switch (dial_state) {
    case NET_DIAL_STARTING: case NET_DIAL_DIALING: case NET_DIAL_NEGOTIATING:
        return NET_ASSOCIATING;
    case NET_DIAL_UP:
        return NET_UP;
    default:
        return NET_DISCONNECTED;
    }
}

void net_modem_status(struct net_status *st)
{
    memset(st, 0, sizeof(*st));
    st->state = (uint32_t)net_modem_state();
    st->kind = GZ_NET_MODEM;
    if (dial_state == NET_DIAL_UP) {
        LOCK_TCPIP_CORE();
        st->addr = ip4_addr_get_u32(netif_ip4_addr(&pppif));
        st->mask = ip4_addr_get_u32(netif_ip4_netmask(&pppif));
        st->gateway = ip4_addr_get_u32(netif_ip4_gw(&pppif));
        st->dns = ip4_addr_get_u32(ip_2_ip4(dns_getserver(0)));
        UNLOCK_TCPIP_CORE();
    }
}
#endif
