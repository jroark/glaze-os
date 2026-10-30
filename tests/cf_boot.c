/* Production socket setup: CE can leave both its clock and COR disabled. */
#include "../kernel/kernel.h"
#include "../kernel/drivers.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define GLAZE_HW_H
static uint32_t companion[0x10000/4], gpio[0x1000/4];
static uint8_t attr[2048], ata[16], ctl;
static uint32_t ctrl, reset, status, bridge, mock_a020, a024, a034, windows;
#define REG32(pa) companion[((pa)&0xffff)/4]
#define REG16(pa) (*(uint16_t *)&gpio[((pa)&0xfff)/4])
#define REG8(pa) attr[((pa)-0x0a00d000)&2047]
#define PCMCIA_CTRL ctrl
#define PCMCIA_RESET reset
#define PCMCIA_STATUS status
#define PCMCIA_CARD_STATUS status
#define PCMCIA_BRIDGE bridge
#define PCMCIA_BRIDGE_EN 4u
#define PCMCIA_A020 mock_a020
#define PCMCIA_A024 a024
#define PCMCIA_A034 a034
#define PCMCIA_WINDOWS windows
#define PCMCIA_A020_CONFIG 8u
#define PCMCIA_CD_MASK 6u
#define PCMCIA_CARD_PRESENT() ((PCMCIA_STATUS & PCMCIA_CD_MASK) == 0)
#define CF_CIS(i) attr[(i)*2]
#define CF_COR attr[0x200]
#define CF_CCSR attr[0x202]
#define CF_ATA(i) ata[i]
#define CF_ATA_CTL ctl
#include "../kernel/drivers/cf.c"
const struct gz_bootinfo *ram_boot;
uint64_t time_us(void) { static uint64_t now; return now+=10000; }
void kprintf(const char *format, ...) { (void)format; }
void kputs(const char *s) { (void)s; }
bool net_present(void) { return false; }
bool net_insert(unsigned m, unsigned p) { (void)m; (void)p; return false; }
void net_removed(void) {}
int copy_to_as(struct addrspace *as, uint32_t a, const void *p, uint32_t n)
{ (void)as; (void)a; (void)p; (void)n; return 0; }
int copy_from_as(struct addrspace *as, void *p, uint32_t a, uint32_t n)
{ (void)as; (void)a; (void)p; (void)n; return 0; }
int main(void)
{
    REG16(0x0f000140)=0xa500; REG16(0x0f000144)=0x5a00;
    mock_a020=0xa5; a024=0x5a; a034=0xff; reset=4;
    socket_power_up();
    assert(REG32(0x0a00111c)==1);
    assert(REG32(0x0a00a010)==0x1073 && REG32(0x0a00a014)==0x1061);
    assert(REG32(0x0a00a018)==0x10b1);
    assert(REG16(0x0f000140)==0xa520 && REG16(0x0f000144)==0x5a20);
    assert(ctrl==11 && reset==4 && windows==1 && bridge==4);
    assert(mock_a020==0xa1 && a024==0x52 && a034==0xcf);
    const int invalid[]={-1,0,0x40,0x80,0xc3,0xff};
    for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        boot_cor=invalid[i]; CF_COR=0;
        assert(configure()); assert(CF_COR==0x43);
        assert((mock_a020&12)==8 && !(a024&12) && !(a034&0x30));
    }
    boot_cor=0x41; assert(configure() && CF_COR==0x41);
    struct proc p={0};
    info.present=true;
    assert(check(&p,false)==GZ_EPERM && check(&p,true)==GZ_EPERM);
    p.flags=PROC_USB;
    assert(!check(&p,false) && check(&p,true)==GZ_EPERM);
    assert(!sys_cf_info(&p,0));
    assert(sys_cf_write(&p,0,1,0)==GZ_EPERM);
    p.flags=PROC_FLASH;
    assert(!check(&p,false) && !check(&p,true));
    struct gz_bootinfo boot={0};ram_boot=&boot;status=PCMCIA_CD_MASK; /* empty socket */
    REG32(0x0a00111c)=0;pcmcia_cold_init(false);assert(!REG32(0x0a00111c));
    boot.flags=GZ_BOOT_FROM_ROM;pcmcia_cold_init(false);assert(REG32(0x0a00111c)==1);
    puts("cf-boot: gated socket setup, disabled/reset COR fallback, valid CE COR retained PASS");
}
