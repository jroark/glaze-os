/* CIS selection must not touch the wrong device's registers. */
#include <glaze/abi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PROC_NET 4
struct proc { unsigned flags; };
static uint8_t cis[256];
static uint32_t bridge, card;
static unsigned reads, last_read, ne_inits, xi_inits, ne_irqs, xi_irqs;
static bool ready;
static uint8_t read_cis(unsigned n)
{
    if (n >= sizeof(cis)) abort();
    reads++; last_read = n;
    return cis[n];
}
#define PCMCIA_BRIDGE bridge
#define PCMCIA_BRIDGE_EN 4
#define PCMCIA_CARD_STATUS card
#define PCMCIA_STATUS_MASK 12
#define PCMCIA_CARD_IN 4
#define CF_CIS(n) read_cis(n)
static unsigned ne_removes, xi_removes;
static void xi825_init(void) { xi_inits++; }
static void ne2000_init(void) { ne_inits++; }
static void xi825_removed(void) { xi_removes++; }
static void ne2000_removed(void) { ne_removes++; }
static void pcmcia_poll(void) {}
static bool xi825_present(void) { return ready; }
static bool ne2000_present(void) { return ready; }
static void xi825_irq(void) { xi_irqs++; }
static void ne2000_irq(void) { ne_irqs++; }
#define STUBS(name,value) \
static int name##_info(struct proc *p, uint32_t b) { (void)p; (void)b; return value; } \
static int name##_bind(struct proc *p, int b) { (void)p; (void)b; return value; } \
static int name##_send(struct proc *p, uint32_t b, uint32_t n) { (void)p; (void)b; (void)n; return value; } \
static int name##_recv(struct proc *p, uint32_t b, uint32_t n) { (void)p; (void)b; (void)n; return value; }
STUBS(xi825, 825)
STUBS(ne2000, 2000)
static int xi825_control(struct proc *p, uint32_t op, uint32_t b) { (void)p; (void)op; (void)b; return 825; }
#define NETDEV_TEST
#include "../kernel/drivers/net.c"
#define CHECK(v) do { if (!(v)) { fprintf(stderr, "line %d: %s\n", __LINE__, #v); exit(1); } } while(0)
static void reset(void)
{
    memset(cis, 0xff, sizeof(cis)); kind=0; card=4; ready=true;
    xi_inits=ne_inits=xi_irqs=ne_irqs=reads=last_read=0;
}
static void identity(unsigned man, unsigned product, unsigned function)
{
    uint8_t b[] = {0x20,4,man,man>>8,product,product>>8,0x21,2,function,0,0xff};
    memcpy(cis, b, sizeof(b));
}
int main(void)
{
    struct proc p={PROC_NET}, unpriv={0};
    reset(); card=0; netdev_init(); CHECK(!reads && !netdev_claimed());
    reset(); identity(0xd601,5,6); netdev_init();
    CHECK(xi_inits==1 && !ne_inits && netdev_claimed() && netdev_present());
    CHECK(sys_net_info(&p,0)==825 && sys_net_send(&p,0,64)==825);
    netdev_irq(); CHECK(xi_irqs==1 && !ne_irqs);
    CHECK(sys_wifi_ctl(&unpriv,0,0)==GZ_EPERM && sys_wifi_ctl(&p,0,0)==825);
    reset(); identity(0x149,0x200,6); netdev_init();
    CHECK(ne_inits==1 && !xi_inits && sys_net_info(&p,0)==2000);
    CHECK(sys_wifi_ctl(&p,0,0)==GZ_ENOENT);
    netdev_irq(); CHECK(ne_irqs==1 && !xi_irqs);
    reset(); identity(0xd601,5,4); netdev_init(); CHECK(!xi_inits && !ne_inits && !netdev_claimed());
    /* Other network cards get the NE2000 driver, which checks the DP8390. */
    reset(); identity(0x1234,5,6); netdev_init(); CHECK(!xi_inits && ne_inits==1);
    /* Hot insertion and removal. */
    reset(); CHECK(netdev_insert(0xd601,5) && xi_inits==1 && netdev_claimed());
    netdev_removed(); CHECK(xi_removes==1 && !netdev_claimed());
    reset(); ready=false; CHECK(!netdev_insert(0x149,0x200) && ne_inits==1);
    netdev_removed(); CHECK(ne_removes==1 && !netdev_claimed());
    reset(); cis[0]=0x20; cis[1]=255; netdev_init(); CHECK(!xi_inits && !ne_inits && last_read==1);
    reset(); memset(cis,0,sizeof(cis)); netdev_init(); CHECK(!netdev_claimed() && reads<=64);
    reset(); identity(0xd601,5,6); ready=false; netdev_init();
    CHECK(netdev_claimed() && !netdev_present());
    puts("netdev: absent, XI825, NE2000, storage, unknown, malformed, failed-init, insertion and removal PASS");
}
