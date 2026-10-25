/* One network card owns the primary socket for the lifetime of a boot. */
#ifndef NETDEV_TEST
#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"
#endif
static unsigned kind;
void netdev_init(void)
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
    /* Never probe the other NIC's registers or an ATA card with NIC writes. */
    if (function != 6) return;
    if (manufacturer == 0xd601 && product == 5) {
        kind = GZ_NET_XI825;
        xi825_init();
    } else if (manufacturer == 0x149 && product == 0x200) {
        kind = GZ_NET_NE2000;
        ne2000_init();
    }
}
bool netdev_claimed(void) { return kind != GZ_NET_NONE; }
bool netdev_present(void)
{
    return kind == GZ_NET_XI825 ? xi825_present() : kind == GZ_NET_NE2000 && ne2000_present();
}
void netdev_irq(void)
{
    if (kind == GZ_NET_XI825) xi825_irq();
    else if (kind == GZ_NET_NE2000) ne2000_irq();
}
int sys_net_info(struct proc *p, uint32_t out)
{
    return kind == GZ_NET_XI825 ? xi825_info(p, out) : ne2000_info(p, out);
}
int sys_net_bind(struct proc *p, int port)
{
    return kind == GZ_NET_XI825 ? xi825_bind(p, port) : ne2000_bind(p, port);
}
int sys_net_send(struct proc *p, uint32_t buf, uint32_t len)
{
    return kind == GZ_NET_XI825 ? xi825_send(p, buf, len) : ne2000_send(p, buf, len);
}
int sys_net_recv(struct proc *p, uint32_t buf, uint32_t len)
{
    return kind == GZ_NET_XI825 ? xi825_recv(p, buf, len) : ne2000_recv(p, buf, len);
}
int sys_wifi_ctl(struct proc *p, uint32_t op, uint32_t buf)
{
    if (!(p->flags & PROC_NET)) return GZ_EPERM;
    return kind == GZ_NET_XI825 ? xi825_control(p, op, buf) : GZ_ENOENT;
}
