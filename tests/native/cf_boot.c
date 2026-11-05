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
static bool require_attribute;
static unsigned bt_inserts, bt_removals;
static uint64_t mock_time;
static bool ata_identify_ready;
static uint8_t cis_byte(unsigned i)
{ return require_attribute && (mock_a020 & 12) != 8 ? 0x2b : attr[i*2]; }
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
#define CF_CIS(i) cis_byte(i)
#define CF_COR attr[0x200]
#define CF_CCSR attr[0x202]
#define CF_ATA(i) ata[i]
#define CF_ATA_DATA16 ata16
#define CF_ATA_CTL ctl
#include "../../kernel/drivers/cf.c"
const struct gz_bootinfo *ram_boot;
uint64_t time_us(void) { if(ata_identify_ready)ata[ATA_STATUS]=ST_DRDY|ST_DRQ;return mock_time+=10000; }
void kprintf(const char *format, ...) { (void)format; }
void kputs(const char *s) { (void)s; }
void bt_insert(void) { bt_inserts++; }
void bt_removed(void) { bt_removals++; }
static uint32_t modem_cor; static unsigned modem_index, modem_inserts, modem_removals;
void modem_insert(uint32_t cor, unsigned index) { modem_cor = cor; modem_index = index; modem_inserts++; }
void modem_removed(void) { modem_removals++; }
bool net_present(void) { return false; }
bool net_insert(unsigned m, unsigned p) { (void)m; (void)p; return false; }
void net_removed(void) {}
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
    /* Reproduce a bus mapping left by a different card: only attribute
     * access reveals the real CIS. Removal and both insertions must reach
     * the dispatcher, without needing a reboot or a second presence edge. */
    booted=true;socket_full=true;status=PCMCIA_CD_MASK;pcmcia_poll();
    unsigned removed=bt_removals, inserted=bt_inserts;
    require_attribute=true;mock_a020=0;status=0x2f9;
    memset(ata,0x5a,sizeof(ata));ctl=0x77;
    pcmcia_poll();
    assert(card_info.kind==BT_CARD_BLUETOOTH && bt_inserts==inserted+1);
    for(unsigned i=0;i<sizeof(ata);i++)assert(ata[i]==0x5a);
    assert(ctl==0x77 && (mock_a020&12)==8);
    status=PCMCIA_CD_MASK;pcmcia_poll();assert(bt_removals==removed+1 && card_info.kind==BT_CARD_ABSENT);
    status=0x2f9;mock_a020=0;pcmcia_poll();assert(card_info.kind==BT_CARD_BLUETOOTH && bt_inserts==inserted+2);
    /* Insertion first exposes garbage, then settles without another edge.
     * Retries are deferred/bounded and never send ATA configuration bytes. */
    status=PCMCIA_CD_MASK;pcmcia_poll();memset(attr,0x2b,sizeof(attr));status=0x2f9;
    pcmcia_poll();assert(cis_retry_at && cis_retries==19 && card_info.kind!=BT_CARD_BLUETOOTH);
    unsigned attempts=inserts;pcmcia_poll();assert(inserts==attempts);
    for(unsigned i=0;i<sizeof(tuples);i++)attr[i*2]=tuples[i];
    mock_time=cis_retry_at;uint64_t before=mock_time;pcmcia_poll();assert(card_info.kind==BT_CARD_BLUETOOTH && !cis_retry_at);
    assert(mock_time-before<100000); /* No 320 ms socket reset on a deferred read. */
    status=PCMCIA_CD_MASK;pcmcia_poll();memset(attr,0x2b,sizeof(attr));status=0x2f9;pcmcia_poll();
    for(unsigned i=0;i<20;i++){mock_time=cis_retry_at;pcmcia_poll();}
    assert(!cis_retry_at && !cis_retries);attempts=inserts;pcmcia_poll();assert(inserts==attempts);
    status=PCMCIA_CD_MASK;pcmcia_poll();assert(!cis_retry_at && !cis_retries);
    status=0x2f9;pcmcia_poll();assert(cis_retry_at);
    status=PCMCIA_CD_MASK;pcmcia_poll();assert(!cis_retry_at && !cis_retries);
    /* A storage card that settles later must publish a new generation so
     * the file server mounts it after having observed the absent state. */
    status=0x2f9;pcmcia_poll();assert(cis_retry_at);
    uint32_t disk_generation=info.generation;
    memset(attr,255,sizeof(attr));attr[0]=TPL_FUNCID;attr[2]=2;attr[4]=FUNCID_FIXED;attr[6]=0;
    ata[0]=1;ata_identify_ready=true;mock_time=cis_retry_at;pcmcia_poll();ata_identify_ready=false;
    assert(info.present && info.sectors && info.generation>disk_generation);
    status=PCMCIA_CD_MASK;pcmcia_poll();memset(attr,0x2b,sizeof(attr));
    /* The inherited-state boot probe must also avoid ATA on a bad CIS. */
    booted=false;status=0x2f9;socket_full=false;memset(ata,0x5a,sizeof(ata));ctl=0x77;probe();
    for(unsigned i=0;i<sizeof(ata);i++)assert(ata[i]==0x5a);
    assert(ctl==0x77);booted=true;
    for(unsigned i=0;i<sizeof(tuples);i++)attr[i*2]=tuples[i];
    read_cis(&c, false);capture_card(&c);require_attribute=false;
    generation=card_info.generation;
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
/* Rockwell RC56LD-PCC, CF56R-BCT-DX, captured on a BE-300 2026-11-04 (MODEM.md). */
static const uint8_t rc56ld[] = {
    0x01,0x02,0x00,0xff,0x15,0x27,0x05,0x00,'R','o','c','k','w','e','l','l',
    ' ','S','e','m','.',' ','S','y','s','.',0x00,'R','C','5','6','L',
    'D','-','P','C','C',0x00,'0','2','1',0x00,'A',0x00,0xff,0x20,0x04,0x13,
    0x00,0x00,0x00,0x21,0x02,0x02,0x00,0x22,0x04,0x00,0x02,0x0f,0x5c,0x22,0x09,0x05,
    0x1f,0x0f,0x00,0x02,0x00,0x00,0x02,0x00,0x22,0x09,0x06,0x1f,0x0f,0x00,0x30,0x00,
    0x00,0x30,0x00,0x22,0x09,0x07,0x1f,0x0f,0x00,0x30,0x00,0x00,0x30,0x00,0x22,0x0d,
    0x02,0x06,0x00,0x3f,0x1c,0x03,0x03,0x0f,0x07,0x00,0x01,0xb5,0xff,0x22,0x09,0x13,
    0x01,0x00,0x1f,0x00,0x7a,0x00,0xb5,0xff,0x22,0x10,0x84,0x06,0x00,0x0b,0x02,0x07,
    0x14,0x00,0x08,0x00,0x04,0x00,0x02,0x00,0x00,0x00,0x1a,0x05,0x01,0x0d,0x20,0x04,
    0x77,0x1b,0x0f,0xc5,0x41,0x9d,0x78,0x65,0x26,0x2e,0x25,0xe7,0x5f,0x23,0x30,0xbc,
    0x86,0x28,0x1b,0x07,0x05,0x08,0xaa,0x60,0xf8,0x03,0x07,0x1b,0x07,0x05,0x08,0xaa,
    0x60,0xf8,0x02,0x07,0x1b,0x07,0x05,0x08,0xaa,0x60,0xe8,0x03,0x07,0x1b,0x07,0x0d,
    0x08,0xaa,0x60,0xe8,0x02,0x07,0xff,
};
static void modem_card_tests(void)
{
    memset(attr, 255, sizeof(attr));
    for (unsigned i = 0; i < sizeof(rc56ld); i++) attr[i*2] = rc56ld[i];
    struct cis c; read_cis(&c, false);
    assert(c.function == 2 && c.manufacturer == 0x13 && c.product == 0 && c.cor == 0x420);
    assert(c.modem && c.uart_index == 5);
    capture_card(&c);
    assert(card_info.kind == BT_CARD_MODEM && strstr(card_info.identification, "RC56LD"));
    assert(modem_inserts == 1 && modem_cor == 0x420 && modem_index == 5);
    /* Never ATA-probed. */
    status = 0x2f9; socket_full = true;
    memset(ata, 0x5a, sizeof(ata)); ctl = 0x77;
    assert(!identify() && ctl == 0x77);
    for (unsigned i = 0; i < sizeof(ata); i++) assert(ata[i] == 0x5a);
    /* Removal and resume release it. */
    uint32_t generation = card_info.generation;
    status = PCMCIA_CD_MASK; probe();
    assert(card_info.kind == BT_CARD_ABSENT && card_info.generation > generation && modem_removals);
    /* A plain serial card (FUNCE type 0 only) is not a modem. */
    c.modem = false; capture_card(&c); assert(card_info.kind == BT_CARD_SERIAL);
    unsigned removals = modem_removals;
    pcmcia_resume(); assert(modem_removals > removals);
    memset(&card_info, 0, sizeof(card_info)); card_info.version = BT_CIS_VERSION;
    socket_full = false;
}

int main(void)
{
    bluetooth_card_tests();
    modem_card_tests();
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
    puts("cf-boot: gated socket setup, COR, BT reinsertion, modem CIS, stale mapping, delayed CIS recovery, bounded retries and no ATA on unreadable cards PASS");
}
