/* Raw-frame interface shared by the NE2000 and Marvell CF station drivers. */
#include "../drivers.h"
#include "../hw.h"
#include "../kernel.h"

#define QUEUE_LEN 8
static enum { NONE, NE2000, WL54CF } device;
static bool claimed;
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
/* Identify the wireless card before NE2000's destructive register probe. */
void net_init(void)
{
    PCMCIA_BRIDGE |= PCMCIA_BRIDGE_EN;
    bool wireless = false;
    if ((PCMCIA_CARD_STATUS & PCMCIA_STATUS_MASK) == PCMCIA_CARD_IN) {
        for (unsigned i = 0; i + 2 < 256;) {
            unsigned tag = CF_CIS(i), n;
            if (tag == 255)
                break;
            if (!tag) {
                i++;
                continue;
            }
            n = CF_CIS(i + 1);
            if (n > 254 - i)
                break;
            if (tag == 0x20 && n == 4 && CF_CIS(i + 2) == 0xdf && CF_CIS(i + 3) == 2 &&
                CF_CIS(i + 4) == 3 && CF_CIS(i + 5) == 0x81)
                wireless = true;
            i += 2 + n;
        }
    }
    if (wireless) {
        claimed = true;
        if (wl54cf_init())
            device = WL54CF;
    } else {
        ne2000_init();
        if (ne2000_present()) {
            device = NE2000;
            claimed = true;
        }
    }
    info.present = device != NONE;
}
/* Claimed remains true if a recognized wireless card failed initialization:
 * the ATA driver must never reset or identify it as a storage device. */
bool net_present(void) { return claimed; }
void net_irq(void)
{
    if (device == NE2000)
        ne2000_irq();
    else if (device == WL54CF)
        wl54cf_irq();
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
    return copy_to_as(&p->as, uout, &info, sizeof(info));
}
int sys_net_bind(struct proc *p, int port)
{
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
    rv = device == WL54CF ? wl54cf_send(tx_buf, len) : ne2000_send(tx_buf, len);
    if (rv >= 0)
        info.tx_frames++;
    return rv;
}
int sys_net_recv(struct proc *p, uint32_t ubuf, uint32_t max)
{
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
