/* Production socket setup: CE can leave both its clock and COR disabled. */
#include "../../kernel/kernel.h"
#include "../../kernel/drivers.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define GLAZE_HW_H
static uint32_t companion[0x10000/4], gpio[0x1000/4];
static uint8_t attr[2048], ata[16], ctl;
static uint16_t ata16;
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
#define PCMCIA0_CARD_PRESENT() ((PCMCIA_STATUS & PCMCIA_CD_MASK) == 0)
/* The jacket's socket (1) is always empty here. */
static uint32_t socket1_detect = 8, socket1_bus, socket1_reg;
#define PCMCIA1_DETECT socket1_detect
#define PCMCIA1_EMPTY 8u
#define PCMCIA1_PRESENCE socket1_reg
#define PCMCIA1_ABSENT 0x40u
#define PCMCIA1_BUS socket1_bus
#define PCMCIA1_BUS_ON 1u
#define PCMCIA1_ID socket1_reg
#define PCMCIA1_ID_VALUE 0x641u
#define PCMCIA1_CHIP socket1_reg
#define PCMCIA1_CHIP_ID 0x7402u
#define PCMCIA1_CTRL socket1_reg
#define PCMCIA1_STATUS socket1_reg
#define PCMCIA1_A520 socket1_reg
#define PCMCIA1_CTRL_UP 0x0Bu
#define PCMCIA0_IO 0x0a00c000u
#define PCMCIA0_ATTR 0x0a00d000u
#define PCMCIA1_IO 0x0b200000u
#define PCMCIA1_ATTR 0x0b600000u
#define CF_CIS(i) attr[(i)*2]
#define CF_COR attr[0x200]
#define CF_CCSR attr[0x202]
#define CF_ATA(i) ata[i]
#define CF_ATA_DATA16 ata16
#define CF_ATA_CTL ctl
#include "../../kernel/drivers/cf.c"
const struct gz_bootinfo *ram_boot;
uint64_t time_us(void) { static uint64_t now; return now+=10000; }
void kprintf(const char *format, ...) { (void)format; }
void kputs(const char *s) { (void)s; }
void bt_insert(void) {}
void bt_removed(void) {}
bool net_present(void) { return false; }
bool net_insert(unsigned m, unsigned p) { (void)m; (void)p; return false; }
void net_removed(void) {}
bool net_insert_modem(void) { return false; }
int copy_to_as(struct addrspace *as, uint32_t a, const void *p, uint32_t n)
{ (void)as; (void)a; (void)p; (void)n; return 0; }
int copy_from_as(struct addrspace *as, void *p, uint32_t a, uint32_t n)
{ (void)as; (void)a; (void)p; (void)n; return 0; }
static void bluetooth_card_tests(void)
{
    /* Physical card's tuples, including exact VERS_1 and COR at E0h. */
    static const uint8_t tuples[] = {
        0x15, 0x2b, 5, 0,
        'B','r','a','i','n',' ','B','o','x','e','s',0,
        'C','F','+',' ','B','l','u','e','t','o','o','t','h','_','C','a','r','d',',',' ','B','C','S','P',0,
        'r','9',0,255,
        0x20,4,0x60,1,0xa0,0xab,0x21,2,2,0,0x1a,5,1,3,0xe0,0,7,255
    };
    memset(attr, 255, sizeof(attr));
    for (unsigned i = 0; i < sizeof(tuples); i++) attr[i*2] = tuples[i];
    struct cis c; read_cis(&c, false); capture_card(&c);
    assert(c.function == 2 && c.manufacturer == 0x160 && c.product == 0xaba0 && c.cor == 0xe0);
    assert(card_info.kind == BT_CARD_BLUETOOTH && card_info.cis_length == sizeof(tuples));
    assert(strstr(card_info.identification, "BCSP"));
    uint32_t generation = card_info.generation;
    struct proc user = {0};
    assert(sys_bt_card_info(&user, 0, sizeof(card_info) - 1) == GZ_EINVAL);
    assert(!sys_bt_card_info(&user, 0, sizeof(card_info)));
    status = 0x2f9; socket_full = true;
    memset(ata, 0x5a, sizeof(ata)); ctl = 0x77;
    assert(!identify());
    for (unsigned i = 0; i < sizeof(ata); i++) assert(ata[i] == 0x5a);
    assert(ctl == 0x77);
    /* Failed reads cannot let a reserved serial card fall into ATA. */
    attr[0] = 255; assert(!identify());
    status = PCMCIA_CD_MASK; probe();
    assert(card_info.kind == BT_CARD_ABSENT && card_info.generation > generation);
    assert(!card_info.cis_length && !card_info.identification[0]);
    /* An unrelated serial card is not advertised as Bluetooth. */
    c.manufacturer = 123; capture_card(&c); assert(card_info.kind == BT_CARD_SERIAL);
    /* Malformed VERS_1 cannot overrun or falsely match the saved identity. */
    attr[0] = 0x15; attr[2] = 255;
    capture_card(&c); assert(!card_info.identification[0]);
    capture_card(&c);
    status = PCMCIA_CD_MASK; pcmcia_resume();
    assert(card_info.kind == BT_CARD_ABSENT && !card_info.manufacturer && !card_info.cis_length);
    memset(&card_info, 0, sizeof(card_info)); card_info.version = BT_CIS_VERSION;
    socket_full = false;
}
int main(void)
{
    bluetooth_card_tests();
    /* Physical OnSpec IDENTIFY provides 256 bytes in 256 low-lane words.
     * Include high bytes in the reconstructed ATA words and capacity. */
    uint16_t expected[256];
    for (unsigned i=0;i<256;++i) expected[i]=(uint16_t)(i*197u);
    expected[0]=0x848a; expected[60]=0xc000; expected[61]=0x72;
    for (unsigned i=0;i<256;++i) {
        io_buf[2*i]=expected[i]&255; io_buf[2*i+1]=expected[i]>>8;
    }
    assert(identify_low_lane(0x02b6,3));
    assert(!identify_low_lane(0x02b6,4) && !identify_low_lane(0x1234,3));
    io_buf[100]|=0x100; assert(!identify_low_lane(0x02b6,3)); io_buf[100]&=255;
    identify_pack_low_lane();
    assert(!memcmp(io_buf,expected,128*sizeof(expected[0])));
    for (unsigned i=128;i<256;++i) assert(!io_buf[i]);
    assert((io_buf[60]|(uint32_t)io_buf[61]<<16)==7520256);
    assert(!identify_low_lane(0x02b6,3)); /* standard packed IDENTIFY */
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
