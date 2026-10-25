/*
 * Network cards and the raw-frame interface shared by their drivers.
 *
 * One card owns the primary socket until it is pulled: found at boot by
 * net_init(), or inserted later (cf.c's pcmcia_poll(), which powers and
 * configures it, then calls net_insert()). A card is identified by its CIS
 * before any of its registers are touched, and only a network card (FUNCID
 * 6) gets a network driver: the XI-825 and WL54-CF by MANFID, anything else
 * the NE2000 driver, which checks the DP8390 and its station address before
 * claiming it (NE2000 clones carry many MANFIDs).
 *
 * The NE2000 and WL54-CF hand received frames to the queue here; the XI-825
 * keeps its own, with its Wi-Fi controls (xi825.c).
 */
#include <glaze/wifi.h>
#ifndef NET_TEST
#include "../drivers.h"
#include "../hw.h"
#include "../kernel.h"
#include "../mips.h"
#endif

#define QUEUE_LEN 8
static unsigned kind;           /* GZ_NET_*: the card that owns the socket */
static bool claimed, wl54cf_up;
static int bound_port;
static struct gz_netif_info info;
static struct {
    uint16_t length;
    uint8_t data[1536];
} queue[QUEUE_LEN];
static uint32_t head, tail;
static uint8_t tx_buf[1536];

void net_set_mac(const uint8_t mac[6]) { memcpy(info.mac, mac, 6); }
void net_rx_drop(void) { info.rx_dropped++; }
void net_overrun(void) { info.overruns++; }
void net_receive(const uint8_t *frame, uint32_t len)
{
    if (len < 14 || len > sizeof(queue[0].data) || head - tail == QUEUE_LEN) {
        net_rx_drop();
        return;
    }
    memcpy(queue[head % QUEUE_LEN].data, frame, len);
    queue[head % QUEUE_LEN].length = (uint16_t)len;
    head++;
    info.rx_frames++;
    if (bound_port > 0)
        ipc_notify(bound_port, GZ_NOTIFY_NET);
}

bool net_up(void)
{
    switch (kind) {
    case GZ_NET_XI825: return xi825_present();
    case GZ_NET_WL54CF: return wl54cf_up;
    case GZ_NET_NE2000: return ne2000_present();
    default: return false;
    }
}

static void select_card(unsigned manufacturer, unsigned product)
{
    claimed = true;
    head = tail = 0;
    memset(&info, 0, sizeof(info));
    if (manufacturer == 0xd601 && product == 5) {
        kind = GZ_NET_XI825;
        xi825_init();
    } else if (manufacturer == 0x02df && product == 0x8103) {
        kind = GZ_NET_WL54CF;
        wl54cf_up = wl54cf_init();
    } else {
        kind = GZ_NET_NE2000;
        ne2000_init();
    }
    info.present = net_up();
    info.kind = info.present ? (uint16_t)kind : GZ_NET_NONE;
}

void net_init(void)
{
    PCMCIA_BRIDGE |= PCMCIA_BRIDGE_EN;
    if ((PCMCIA_CARD_STATUS & PCMCIA_STATUS_MASK) != PCMCIA_CARD_IN)
        return;
    unsigned function = 0xff, manufacturer = 0, product = 0;
    for (unsigned i = 0, n = 0; n < 64 && i + 2 < 0x100; n++) {
        unsigned code = CF_CIS(i);
        if (code == 0xff) break;
        if (!code) { i++; continue; }
        unsigned len = CF_CIS(i + 1);
        if (len == 0xff || i + 2 + len > 0x100) break;
        if (code == 0x21 && len >= 2) function = CF_CIS(i + 2);
        if (code == 0x20 && len >= 4) {
            manufacturer = CF_CIS(i + 2) | CF_CIS(i + 3) << 8;
            product = CF_CIS(i + 4) | CF_CIS(i + 5) << 8;
        }
        i += 2 + len;
    }
    /* Never probe a storage card's registers with NIC writes. */
    if (function == 6)
        select_card(manufacturer, product);
}

bool net_insert(unsigned manufacturer, unsigned product)
{
    select_card(manufacturer, product);
    return net_up();
}

/* The card was pulled: its driver forgets it, and its frames go. */
void net_removed(void)
{
    if (kind == GZ_NET_XI825) xi825_removed();
    else if (kind == GZ_NET_WL54CF) wl54cf_removed();
    else if (kind == GZ_NET_NE2000) ne2000_removed();
    kind = GZ_NET_NONE;
    claimed = wl54cf_up = false;
    head = tail = 0;
    info.present = 0;
    info.kind = GZ_NET_NONE;
    if (bound_port > 0)
        ipc_notify(bound_port, GZ_NOTIFY_NET);
}

/* Claimed stays true if a recognized card failed initialization: the ATA
 * driver must never reset or identify it as a storage device. */
bool net_present(void) { return claimed; }

void net_irq(void)
{
    /* From the tick and GIRQ0 alike: a pulled card reads 0xFF everywhere,
     * which a driver would take for every event at once. pcmcia_poll()
     * notices the removal itself. */
    if ((PCMCIA_CARD_STATUS & PCMCIA_STATUS_MASK) != PCMCIA_CARD_IN)
        return;
    if (kind == GZ_NET_XI825) xi825_irq();
    else if (kind == GZ_NET_WL54CF) wl54cf_irq();
    else if (kind == GZ_NET_NE2000) ne2000_irq();
}

static int check(struct proc *p)
{
    if (!(p->flags & PROC_NET))
        return GZ_EPERM;
    return info.present ? 0 : GZ_ENOENT;
}

int sys_net_info(struct proc *p, uint32_t uout)
{
    if (!(p->flags & PROC_NET))
        return GZ_EPERM;
    pcmcia_poll();          /* the network server's look at the socket */
    if (kind == GZ_NET_XI825)
        return xi825_info(p, uout);
    return copy_to_as(&p->as, uout, &info, sizeof(info));
}

int sys_net_bind(struct proc *p, int port)
{
    if (kind == GZ_NET_XI825)
        return xi825_bind(p, port);
    int rv = check(p);
    if (rv)
        return rv;
    bound_port = port;
    if (head != tail)
        ipc_notify(port, GZ_NOTIFY_NET);
    return 0;
}

int sys_net_send(struct proc *p, uint32_t ubuf, uint32_t len)
{
    if (kind == GZ_NET_XI825)
        return xi825_send(p, ubuf, len);
    int rv = check(p);
    if (rv)
        return rv;
    if (len < 14 || len > GZ_ETH_FRAME_MAX)
        return GZ_EINVAL;
    if (copy_from_as(&p->as, tx_buf, ubuf, len))
        return GZ_EINVAL;
    if (len < 60) {
        memset(tx_buf + len, 0, 60 - len);
        len = 60;
    }
    rv = kind == GZ_NET_WL54CF ? wl54cf_send(tx_buf, len) : ne2000_send(tx_buf, len);
    if (rv >= 0)
        info.tx_frames++;
    return rv;
}

int sys_net_recv(struct proc *p, uint32_t ubuf, uint32_t max)
{
    if (kind == GZ_NET_XI825)
        return xi825_recv(p, ubuf, max);
    int rv = check(p);
    if (rv)
        return rv;
    if (head == tail)
        return 0;
    uint32_t n = MIN(queue[tail % QUEUE_LEN].length, max);
    if (copy_to_as(&p->as, ubuf, queue[tail % QUEUE_LEN].data, n))
        return GZ_EINVAL;
    tail++;
    return (int)n;
}

int sys_wifi_ctl(struct proc *p, uint32_t op, uint32_t address)
{
    if (!(p->flags & PROC_NET))
        return GZ_EPERM;
    if (kind == GZ_NET_XI825)
        return xi825_control(p, op, address);
    if (kind != GZ_NET_WL54CF || !info.present)
        return GZ_ENOENT;
    union { struct wifi_status status; struct wifi_config config; struct wifi_request request;
            struct wifi_key key; } data;
    unsigned n = op == WIFI_CONFIGURE ? sizeof(data.config) :
                 op == WIFI_SET_KEY ? sizeof(data.key) :
                 op == WIFI_CONNECT ? sizeof(data.request) : 0;
    memset(&data, 0, sizeof(data));
    if (n && copy_from_as(&p->as, &data, address, n))
        return GZ_EINVAL;
    /* Commands and the runtime IRQ share the command/response FIFO. */
#ifndef NET_TEST
    uint32_t flags = irq_save();
#endif
    int rv = wl54cf_control(op, &data);
    if (op == WIFI_CONNECT || op == WIFI_CONFIGURE || op == WIFI_DISCONNECT)
        head = tail = 0;
#ifndef NET_TEST
    irq_restore(flags);
#endif
    if (!rv && op == WIFI_STATUS)
        rv = copy_to_as(&p->as, address, &data.status, sizeof(data.status));
    memset(&data, 0, sizeof(data));
    return rv;
}
