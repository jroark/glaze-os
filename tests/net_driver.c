/* Exercise production network dispatch/syscalls with fake socket/backend I/O. */
#include "../kernel/drivers.h"
#include "../kernel/kernel.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define GLAZE_HW_H
static uint32_t bridge, socket_status;
static uint8_t cis[256];
#define PCMCIA_BRIDGE bridge
#define PCMCIA_BRIDGE_EN 4
#define PCMCIA_CARD_STATUS socket_status
#define PCMCIA_STATUS_MASK 12
#define PCMCIA_CARD_IN 4
#define CF_CIS(i) cis[i]
#define NETDRV_TEST
#include "../kernel/drivers/net.c"

static unsigned ne_probes, wl_probes, ne_irqs, wl_irqs, notices;
static bool ne_ok, wl_ok, copy_fail;
static uint8_t userspace[4096];
static uint8_t sent[1600];
static unsigned sent_len;
static const uint8_t address[6] = {2, 3, 4, 5, 6, 7};
void ne2000_init(void)
{
    ne_probes++;
    if (ne_ok)
        net_set_mac(address);
}
bool ne2000_present(void) { return ne_ok; }
bool wl54cf_init(void)
{
    wl_probes++;
    if (wl_ok)
        net_set_mac(address);
    return wl_ok;
}
void ne2000_irq(void) { ne_irqs++; }
void wl54cf_irq(void) { wl_irqs++; }
int ne2000_send(const uint8_t *p, uint32_t n)
{
    memcpy(sent, p, n);
    sent_len = n;
    return (int)n;
}
int wl54cf_send(const uint8_t *p, uint32_t n) { return ne2000_send(p, n); }
int ipc_notify(int port, uint32_t bits)
{
    assert(port == 7 && bits == GZ_NOTIFY_NET);
    notices++;
    return 0;
}
int copy_to_as(struct addrspace *as, uint32_t addr, const void *p, uint32_t n)
{
    if (copy_fail || addr > sizeof(userspace) || n > sizeof(userspace) - addr)
        return -1;
    memcpy(userspace + addr, p, n);
    return 0;
}
int copy_from_as(struct addrspace *as, void *p, uint32_t addr, uint32_t n)
{
    if (copy_fail || addr > sizeof(userspace) || n > sizeof(userspace) - addr)
        return -1;
    memcpy(p, userspace + addr, n);
    return 0;
}
static void reset(bool wireless)
{
    device = NONE;
    claimed = false;
    bound_port = 0;
    head = tail = 0;
    memset(&info, 0, sizeof(info));
    memset(cis, 255, sizeof(cis));
    socket_status = wireless ? 4 : 0;
    if (wireless) {
        uint8_t id[] = {0x20, 4, 0xdf, 2, 3, 0x81, 255};
        memcpy(cis, id, sizeof(id));
    }
    ne_probes = wl_probes = ne_irqs = wl_irqs = notices = 0;
    ne_ok = wl_ok = false;
    copy_fail = false;
}
int main(void)
{
    struct proc p = {0};
    reset(false);
    net_init();
    assert(!net_present());
    assert(ne_probes == 1);
    assert(sys_net_info(&p, 0) == GZ_EPERM);
    assert(sys_net_send(&p, 0, 60) == GZ_EPERM);
    assert(sys_net_bind(&p, 7) == GZ_EPERM);
    assert(sys_net_recv(&p, 0, 60) == GZ_EPERM);
    p.flags = PROC_NET;
    assert(sys_net_info(&p, 0) == 0);
    assert(!((struct gz_netif_info *)userspace)->present);
    assert(sys_net_send(&p, 0, 60) == GZ_ENOENT);
    reset(true);
    net_init();
    assert(net_present() && !info.present);
    assert(wl_probes == 1 && ne_probes == 0); /* recognized failed Wi-Fi is never ATA/NE2000 */
    for (int wireless = 0; wireless < 2; wireless++) {
        reset(wireless);
        ne_ok = wl_ok = true; socket_status = PCMCIA_CARD_IN;
        net_init();
        assert(info.present && !memcmp(info.mac, address, 6));
        assert(ne_probes == (unsigned)!wireless && wl_probes == (unsigned)wireless);
        net_irq();
        assert(ne_irqs == (unsigned)!wireless && wl_irqs == (unsigned)wireless);
        assert(sys_net_bind(&p, 7) == 0);
        uint8_t frame[60] = {0};
        frame[59] = 0x6d;
        for (int i = 0; i < 8; i++)
            net_receive(frame, sizeof(frame));
        net_receive(frame, sizeof(frame));
        assert(info.rx_frames == 8 && info.rx_dropped == 1);
        assert(notices == 8);
        copy_fail = true;
        assert(sys_net_recv(&p, 0, 60) == GZ_EINVAL);
        assert(tail == 0);
        assert(sys_net_send(&p, 0, 60) == GZ_EINVAL);
        copy_fail = false;
        for (int i = 0; i < 8; i++) {
            assert(sys_net_recv(&p, 0, 60) == 60);
            assert(userspace[59] == 0x6d);
        }
        assert(sys_net_recv(&p, 0, 60) == 0);
        assert(sys_net_send(&p, 0, 13) == GZ_EINVAL);
        assert(sys_net_send(&p, 0, 1515) == GZ_EINVAL);
        memset(userspace, 0x51, 60);
        assert(sys_net_send(&p, 0, 14) == 60);
        assert(sent_len == 60 && sent[13] == 0x51 && sent[14] == 0 && sent[59] == 0);
        assert(info.tx_frames == 1);
        net_overrun();
        assert(info.overruns == 1);
        net_receive(frame, 13);
        assert(info.rx_dropped == 2);
    }
    socket_status=0;
    net_irq();
    assert(!info.present && !claimed && head==tail);
    assert(sys_net_send(&p,0,60)==GZ_ENOENT);
    assert(sys_wifi_ctl(&p,WIFI_STATUS,0)==GZ_ENOENT);
    p.flags=0;assert(sys_wifi_ctl(&p,WIFI_SCAN,0)==GZ_EPERM);
    puts("net driver: dispatch, queues, permissions and removal PASS");
    return 0;
}

void xi825_init(void) {}
void xi825_irq(void) {}
bool xi825_present(void) { return false; }
int xi825_info(struct proc *p, uint32_t a) { return GZ_ENOENT; }
int xi825_bind(struct proc *p, int a) { return GZ_ENOENT; }
int xi825_send(struct proc *p, uint32_t a, uint32_t n) { return GZ_ENOENT; }
int xi825_recv(struct proc *p, uint32_t a, uint32_t n) { return GZ_ENOENT; }
int xi825_control(struct proc *p, uint32_t op, uint32_t a) { return GZ_ENOENT; }
int wl54cf_control(uint32_t op, void *data) { return GZ_ENOENT; }

void xi825_removed(void) {}
void wl54cf_removed(void) {}
