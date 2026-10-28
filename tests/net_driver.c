/* Exercise production network dispatch/syscalls with fake socket/backend I/O:
 * CIS selection (never a storage card, XI-825 and WL54-CF by MANFID, other
 * network cards to the NE2000 driver), the shared frame queue, hot insertion
 * and removal. */
#include "../kernel/drivers.h"
#include "../kernel/kernel.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GLAZE_HW_H
static uint32_t bridge, socket_status;
static uint8_t cis[256];
#define PCMCIA_BRIDGE bridge
#define PCMCIA_BRIDGE_EN 4
#define PCMCIA_CARD_STATUS socket_status
#define PCMCIA_STATUS_MASK 12
#define PCMCIA_CARD_IN 4
static unsigned reads;
static uint8_t read_cis(unsigned i)
{
    assert(i < sizeof(cis));
    reads++;
    return cis[i];
}
#define CF_CIS(i) read_cis(i)
#define NET_TEST
#include "../kernel/drivers/net.c"

static unsigned ne_probes, wl_probes, xi_probes, ne_irqs, wl_irqs, xi_irqs, notices, removes;
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
void ne2000_removed(void) { removes++; }
void wl54cf_removed(void) { removes++; }
void pcmcia_poll(void) {}
void xi825_init(void) { xi_probes++; }
void xi825_removed(void) { removes++; }
void xi825_irq(void) { xi_irqs++; }
bool xi825_present(void) { return ne_ok; }
int xi825_info(struct proc *p, uint32_t b) { (void)p; (void)b; return 825; }
int xi825_bind(struct proc *p, int b) { (void)p; (void)b; return 825; }
int xi825_send(struct proc *p, uint32_t b, uint32_t n) { (void)p; (void)b; (void)n; return 825; }
int xi825_recv(struct proc *p, uint32_t b, uint32_t n) { (void)p; (void)b; (void)n; return 825; }
int xi825_control(struct proc *p, uint32_t op, uint32_t b) { (void)p; (void)op; (void)b; return 825; }
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
static bool ne_start(void) { ne2000_init(); return ne2000_present(); }
static bool wl_present(void) { return wl_ok; }
static bool xi_start(void) { xi825_init(); return xi825_present(); }
static const struct net_driver fake_ne = {
    .magic = NET_DRIVER_MAGIC, .abi = NET_DRIVER_ABI, .size = sizeof(struct net_driver), .kind = GZ_NET_NE2000,
    .init = ne_start, .present = ne2000_present, .irq = ne2000_irq, .stop = ne2000_removed, .send_frame = ne2000_send
};
static const struct net_driver fake_wl = {
    .magic = NET_DRIVER_MAGIC, .abi = NET_DRIVER_ABI, .size = sizeof(struct net_driver), .kind = GZ_NET_WL54CF,
    .init = wl54cf_init, .present = wl_present, .irq = wl54cf_irq, .stop = wl54cf_removed,
    .send_frame = wl54cf_send, .control = wl54cf_control
};
static const struct net_driver fake_xi = {
    NET_DRIVER_MAGIC, NET_DRIVER_ABI, sizeof(struct net_driver), GZ_NET_XI825,
    xi_start, xi825_present, xi825_irq, xi825_removed, NULL, NULL,
    xi825_info, xi825_bind, xi825_send, xi825_recv, xi825_control
};
static unsigned loads, allocations;
static void *allocation;
static unsigned remaining;
static bool load_fail;
void *page_alloc_n(uint32_t n)
{
    assert(!allocation);
    allocation = calloc(n, PAGE_SIZE); assert(allocation);
    remaining = n; allocations += n;
    return allocation;
}
void page_free(void *p)
{
    assert(p >= allocation && (uint8_t *)p < (uint8_t *)allocation + QUEUE_PAGES * PAGE_SIZE);
    allocations--; if (!--remaining) { free(allocation); allocation = NULL; }
}
int module_load(const char *path, const char *entry, struct module *m)
{
    loads++;
    if (load_fail) return GZ_ECORRUPT;
    const struct net_driver *d = strstr(path, "xi825") ? &fake_xi : strstr(path, "wl54cf") ? &fake_wl : &fake_ne;
    m->base = m->entry = (void *)d; m->bytes = sizeof(*d); m->pages = 1;
    return 0;
}
void module_unload(struct module *m) { memset(m, 0, sizeof(*m)); }
bool module_contains(const struct module *m, const void *p, uint32_t n) { return p != NULL; }
bool module_executable(const struct module *m, const void *p) { return p != NULL; }
void kprintf(const char *format, ...) {}

/* A card with MANFID man:product and FUNCID function in the socket. */
static void card(unsigned man, unsigned product, unsigned function)
{
    uint8_t id[] = {0x20, 4, man, man >> 8, product, product >> 8, 0x21, 2, function, 0, 255};
    memcpy(cis, id, sizeof(id));
    socket_status = 4;
}
static void reset(int which)  /* 0 none, 1 WL54-CF, 2 NE2000 */
{
    if (claimed || driver) net_removed();
    kind = GZ_NET_NONE;
    claimed = online = false;
    bound_port = 0;
    head = tail = 0;
    memset(&info, 0, sizeof(info));
    memset(cis, 255, sizeof(cis));
    socket_status = 0;
    if (which == 1)
        card(0x02df, 0x8103, 6);
    else if (which == 2)
        card(0x149, 0x200, 6);
    ne_probes = wl_probes = xi_probes = ne_irqs = wl_irqs = xi_irqs = notices = removes = reads = 0;
    ne_ok = wl_ok = false;
    copy_fail = false;
    loads = 0; load_fail = false;
}
int main(void)
{
    struct proc p = {0};
    reset(0);
    net_init(); p.net_generation = generation;
    assert(!net_present() && !reads && !loads && !allocations && !net_driver_pages());
    assert(sys_net_info(&p, 0) == GZ_EPERM);
    assert(sys_net_send(&p, 0, 60) == GZ_EPERM);
    assert(sys_net_bind(&p, 7) == GZ_EPERM);
    assert(sys_net_recv(&p, 0, 60) == GZ_EPERM);
    p.flags = PROC_NET; p.net_generation = generation;
    assert(net_handle(&p) > 0);
    assert(sys_net_info(&p, 0) == 0);
    assert(!((struct gz_netif_info *)userspace)->present);
    assert(sys_net_send(&p, 0, 60) == GZ_ENOENT);
    reset(1);
    net_init(); p.net_generation = generation;
    assert(net_present() && !info.present);
    assert(wl_probes == 1 && ne_probes == 0); /* recognized failed Wi-Fi is never ATA/NE2000 */
    for (int wireless = 0; wireless < 2; wireless++) {
        reset(wireless ? 1 : 2);
        ne_ok = wl_ok = true;
        net_init(); p.net_generation = generation;
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
    p.flags = PROC_NET; p.net_generation = generation;

    /* Storage, and cards whose CIS doesn't parse, are never probed. */
    reset(0); card(0x45, 0x401, 4); net_init();
    assert(!net_present() && !ne_probes && !wl_probes && !xi_probes);
    reset(0); socket_status = 4; cis[0] = 0x20; cis[1] = 255; net_init();
    assert(!net_present() && reads == 2);
    reset(0); socket_status = 4; memset(cis, 0, sizeof(cis)); net_init();
    assert(!net_present() && reads <= 64);
    /* Network cards other than the Wi-Fi ones get the NE2000 driver. */
    reset(0); card(0x1234, 5, 6); net_init();
    assert(net_present() && ne_probes == 1 && !wl_probes && !xi_probes);
    /* The XI-825 keeps its own syscalls and has Wi-Fi controls. */
    reset(0); card(0xd601, 5, 6); ne_ok = true; net_init(); p.net_generation = generation;
    assert(xi_probes == 1 && net_up() && info.kind == GZ_NET_XI825);
    assert(sys_net_info(&p, 0) == 825 && sys_net_send(&p, 0, 60) == 825);
    assert(sys_wifi_ctl(&p, 0, 0) == 825);
    net_irq(); assert(xi_irqs == 1 && !ne_irqs);
    socket_status = 0; net_irq(); assert(xi_irqs == 1);    /* pulled */
    reset(2); ne_ok = true; net_init(); p.net_generation = generation;
    assert(sys_wifi_ctl(&p, 0, 0) == GZ_ENOENT);
    /* Hot insertion and removal. */
    reset(0); ne_ok = true;
    assert(net_insert(0x149, 0x200) && net_present() && info.kind == GZ_NET_NE2000);
    assert(sys_net_send(&p, 0, 60) == GZ_EDEAD); /* old server cannot drive a replacement */
    p.net_generation = generation;
    assert(sys_net_bind(&p, 7) == 0);
    uint8_t frame[60] = {0};
    net_receive(frame, sizeof(frame));
    net_removed();
    assert(removes == 1 && !net_present() && !net_up() && head == tail);
    assert(sys_net_send(&p, 0, 60) == GZ_EDEAD);
    assert(sys_wifi_ctl(&p, WIFI_STATUS, 0) == GZ_EDEAD);
    p.flags = 0;
    assert(sys_wifi_ctl(&p, WIFI_SCAN, 0) == GZ_EPERM);
    p.flags = PROC_NET; p.net_generation = generation;
    reset(0);
    assert(!net_insert(0x02df, 0x8103) && net_present());   /* failed: still claimed */
    net_removed();
    assert(!net_present());
    net_removed();
    assert(!allocations && !net_driver_pages());
    reset(2); ne_ok = true; load_fail = true; net_init();
    assert(net_present() && !net_up() && !ne_probes && !allocations && !net_driver_pages());
    puts("net driver: dispatch, storage, XI-825, frames, Wi-Fi controls, insertion and removal OK");
    return 0;
}

int wl54cf_control(uint32_t op, void *data) { (void)op; (void)data; return GZ_ENOENT; }
