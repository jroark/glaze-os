/*
 * CompactFlash storage card: ATA in the PCMCIA card window, for the file
 * server (servers/fs), which runs FatFs on it as /Card.
 *
 * The card shares the socket with network cards; only one is ever
 * present. Transfers are polled PIO with READ/WRITE SECTORS in LBA28 mode
 * (CF+ and CompactFlash spec 6.2, ATA-4 commands 20h, 30h, ECh). The
 * emulator raises no interrupt when a command completes (be300
 * src/hw/cf.c), and polling is what the Linux port does too, so the
 * kernel never waits on GIRQ0 for the card.
 *
 * There is no hot-plug interrupt: SYS_CF_INFO samples the socket status
 * and re-identifies the card when it changes, bumping a generation count
 * the file server compares to notice a swapped card. At boot the card is
 * one GlazeBoot came from, configured by Windows CE. NAND boot and later
 * insertions explicitly set up the socket and card using the sequences from
 * the stock SPL and pcmcia.dll. RAM handoff retains CE's live socket and COR.
 */

#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"
#include "../proc.h"
#include <glaze/bootinfo.h>
#include <glaze/bluetooth.h>

/* Taskfile registers (offsets from CF_ATA_BASE). */
#define ATA_ERROR       1       /* read */
#define ATA_FEATURES    1       /* write */
#define ATA_COUNT       2
#define ATA_LBA0        3
#define ATA_LBA1        4
#define ATA_LBA2        5
#define ATA_DEVICE      6
#define ATA_STATUS      7       /* read */
#define ATA_COMMAND     7       /* write */

#define ST_ERR          0x01
#define ST_DRQ          0x08
#define ST_DF           0x20
#define ST_DRDY         0x40
#define ST_BSY          0x80

#define CTL_NIEN        0x02    /* no interrupts: everything is polled */
#define CTL_SRST        0x04

#define DEV_LBA         0xE0

#define CMD_READ        0x20
#define CMD_WRITE       0x30
#define CMD_IDENTIFY    0xEC
#define CMD_SET_FEATURES 0xEF
#define FEATURE_8BIT    0x01    /* CF spec: 8-bit data transfers */

/* CIS tuples (PC Card spec, "Card Information Structure"). */
#define TPL_NULL        0x00
#define TPL_FUNCID      0x21
#define TPL_END         0xFF
#define FUNCID_FIXED    0x04

#define SECTOR          512
#define MAX_SECTORS     (GZ_CF_IO_MAX / SECTOR)
#define TIMEOUT_US      2000000u    /* reset and spin-up can be slow */

static struct gz_cf_info info;
static struct bt_card_info card_info = { .version = BT_CIS_VERSION };
static bool socket_full;         /* the socket status at the last probe */
static bool booted;              /* cf_init has run: later insertions are hot */
static uint16_t io_buf[GZ_CF_IO_MAX / 2];
static int boot_cor = -1;        /* the COR Windows CE set, if it was readable */
/* Socket history for HWDUMP=1 diagnostics (kernel/hwdump.c, cf_debug). */
static uint32_t cold_status = ~0u, cold_inserts, inserts, probes, removals;

static bool wait_status(uint8_t mask, uint8_t want)
{
    uint64_t t0 = time_us();

    for (;;) {
        uint8_t st = CF_ATA(ATA_STATUS);
        if (st != 0xFF && !(st & ST_BSY) && (st & mask) == want)
            return true;
        if (time_us() - t0 > TIMEOUT_US)
            return false;
    }
}

static bool card_inserted(void)
{
    return PCMCIA_CARD_PRESENT();
}

/*
 * What the card information structure says (PC Card spec, "Card
 * Information Structure"): FUNCID, MANFID, CONFIG's configuration register
 * address, and the configuration index to use. Tuples are read only below
 * attribute address 0x200: above it, 0x0A00D200 is the NAND's direct-I/O
 * port unless A020 bit 3 maps the configuration registers (hw.h).
 */
#define CIS_LIMIT       0x100u          /* tuple bytes: attribute 0..0x1FF */
#define TPL_MANFID      0x20
#define TPL_CONFIG      0x1A
#define TPL_CFTABLE     0x1B
#define FUNCID_NETWORK  0x06
#define FUNCID_SERIAL   0x02
#define TPL_FUNCE       0x22
#define UART_IO_BASE    0x3F8u          /* COM1, where cfuart.ko looks */
#define NET_IO_BASE     0x300u          /* NE_REG and the XI-825's window */

struct cis {
    int function;                       /* -1: no FUNCID */
    unsigned manufacturer, product;
    uint32_t cor;                       /* attribute address, 0: none */
    int index, io_index;                /* default entry; one decoding 0x300 */
    int uart_index;                     /* an entry decoding UART_IO_BASE */
    bool modem;                         /* serial FUNCE names a modem function */
};

/* A CFTABLE_ENTRY's index and whether it can decode NET_IO_BASE: a range
 * starting there, or address lines only (the window picks the base). */
static void cis_entry(unsigned at, unsigned len, struct cis *c)
{
    unsigned end = at + len, p = at;
#define NEXT() (p < end ? CF_CIS(p++) : 0)
    uint8_t indx = NEXT();
    int index = indx & 0x3F;

    if (c->index < 0 || (indx & 0x40))
        c->index = index;
    if (indx & 0x80)
        (void)NEXT();                   /* interface */
    uint8_t fs = NEXT();
    for (unsigned n = fs & 3; n; n--) {  /* power descriptors */
        uint8_t sel = NEXT();
        for (int bit = 0; bit < 7; bit++)
            if (sel & (1u << bit))
                while ((NEXT() & 0x80) && p < end)
                    ;
    }
    if (fs & 0x04) {                    /* timing */
        uint8_t t = NEXT();
        if ((t & 3) != 3)
            while ((NEXT() & 0x80) && p < end)
                ;
        if (((t >> 2) & 7) != 7)
            while ((NEXT() & 0x80) && p < end)
                ;
        if (((t >> 5) & 7) != 7)
            while ((NEXT() & 0x80) && p < end)
                ;
    }
    if (!(fs & 0x08))
        return;
    uint8_t io = NEXT();
    bool decodes = !(io & 0x80) && (io & 0x1F) >= 5;
    /* Eight registers: three address lines and the window picks the base. */
    bool uart = !(io & 0x80) && (io & 0x1F) >= 3;
    if (io & 0x80) {
        uint8_t r = NEXT();
        unsigned asz = (r >> 4) & 3, lsz = (r >> 6) & 3;
        asz = asz == 3 ? 4 : asz;
        lsz = lsz == 3 ? 4 : lsz;
        for (unsigned n = (r & 0x0F) + 1; n; n--) {
            uint32_t base = 0;
            for (unsigned b = 0; b < asz; b++)
                base |= (uint32_t)NEXT() << (8 * b);
            for (unsigned b = 0; b < lsz; b++)
                (void)NEXT();
            decodes = decodes || base == NET_IO_BASE;
            uart = uart || base == UART_IO_BASE;
        }
    }
    if (decodes && c->io_index < 0)
        c->io_index = index;
    if (uart && c->uart_index < 0)
        c->uart_index = index;
#undef NEXT
}

static void read_cis(struct cis *c)
{
    unsigned i = 0;

    memset(c, 0, sizeof(*c));
    c->function = c->index = c->io_index = c->uart_index = -1;
    for (int n = 0; n < 64 && i + 2 < CIS_LIMIT; n++) {
        uint8_t code = CF_CIS(i);
        if (code == TPL_END)
            break;
        if (code == TPL_NULL) {
            i++;
            continue;
        }
        unsigned len = CF_CIS(i + 1);
        if (len == 0xFF || i + 2 + len > CIS_LIMIT)
            break;
        unsigned at = i + 2;
        if (code == TPL_FUNCID && len >= 1)
            c->function = CF_CIS(at);
        else if (code == TPL_MANFID && len >= 4) {
            c->manufacturer = CF_CIS(at) | (unsigned)CF_CIS(at + 1) << 8;
            c->product = CF_CIS(at + 2) | (unsigned)CF_CIS(at + 3) << 8;
        } else if (code == TPL_CONFIG && len >= 3) {
            unsigned rasz = (CF_CIS(at) & 3) + 1;
            c->cor = 0;
            for (unsigned b = 0; b < rasz && 2 + b < len; b++)
                c->cor |= (uint32_t)CF_CIS(at + 2 + b) << (8 * b);
        } else if (code == TPL_CFTABLE && len >= 1) {
            cis_entry(at, len, c);
        } else if (code == TPL_FUNCE && len >= 1 && c->function == FUNCID_SERIAL) {
            /* PC Card Standard, serial-port function extensions: the low
             * nibble of TPLFE_TYPE is 1 (modem), 2 (data modem) or 5 (data
             * modem capabilities) on a modem; 0 is the plain UART. */
            unsigned type = CF_CIS(at) & 0x0F;
            if (type == 1 || type == 2 || type == 5)
                c->modem = true;
        }
        i = at + len;
    }
}

/* Cache tuple evidence while attribute memory is mapped. Queries never
 * reconfigure the socket, power-cycle a card, or touch UART/ATA registers.
 * PC Card Standard: VERS_1 (15h), FUNCID serial (2), MANFID and CONFIG.
 */
static void capture_card(const struct cis *c)
{
    uint32_t generation = card_info.generation + 1;
    memset(&card_info, 0, sizeof(card_info));
    card_info.version = BT_CIS_VERSION;
    card_info.generation = generation;
    card_info.kind = c->function == 2 ? BT_CARD_SERIAL : BT_CARD_OTHER;
    card_info.function = (uint32_t)c->function;
    card_info.manufacturer = c->manufacturer;
    card_info.product = c->product;
    card_info.cor = c->cor;
    card_info.config_index = c->index;
    unsigned used = 0;
    for (unsigned i = 0; i < CIS_LIMIT; i++)
        card_info.cis[i] = CF_CIS(i);
    for (unsigned i = 0; i < CIS_LIMIT;) {
        unsigned code = card_info.cis[i++];
        card_info.cis_length = i;
        if (code == TPL_END) break;
        if (code == TPL_NULL) continue;
        if (i >= CIS_LIMIT) break;
        unsigned len = card_info.cis[i++];
        if (len == 255 || len > CIS_LIMIT - i) break;
        if (code == 0x15 && len >= 2) {
            for (unsigned j = i + 2; j < i + len && card_info.cis[j] != 255; j++) {
                unsigned ch = card_info.cis[j];
                if (used + 1 < sizeof(card_info.identification))
                    card_info.identification[used++] = ch >= 32 && ch < 127 ? ch : ' ';
            }
        }
        i += len;
        card_info.cis_length = i;
    }
    /* Physical SMART card, captured over Glaze USB 2026-11-02:
     * MANFID 0160:aba0, FUNCID serial, VERS_1 explicitly names BCSP.
     * UART divisor 1/8E1 produces BCSP SYNC replies (BLUETOOTH.md).
     * The matched card loads the UART module; BCSP/HCI run in userspace. */
    if (c->function == 2 && c->manufacturer == 0x0160 && c->product == 0xaba0 &&
        strstr(card_info.identification, "CF+ Bluetooth_Card, BCSP"))
        card_info.kind = BT_CARD_BLUETOOTH;
    /* Modems: any serial card whose FUNCE names a modem and that has an
     * entry decoding COM1 (verified card: Rockwell RC56LD, MODEM.md). */
    else if (c->function == FUNCID_SERIAL && c->modem && c->cor && c->uart_index >= 0)
        card_info.kind = BT_CARD_MODEM;
    if (card_info.kind == BT_CARD_BLUETOOTH) bt_insert();
    if (card_info.kind == BT_CARD_MODEM) modem_insert(c->cor, (unsigned)c->uart_index);
}

/* A serial card owns the socket: never ATA-probe or reset it. */
static bool serial_card(void)
{
    return card_info.kind == BT_CARD_SERIAL || card_info.kind == BT_CARD_BLUETOOTH ||
           card_info.kind == BT_CARD_MODEM;
}

static void forget_card(void)
{
    bt_removed();
    modem_removed();
    uint32_t generation = card_info.generation + 1;
    memset(&card_info, 0, sizeof(card_info));
    card_info.version = BT_CIS_VERSION;
    card_info.generation = generation;
}

int sys_bt_card_info(struct proc *p, uint32_t out, uint32_t size)
{
    if (size != sizeof(card_info)) return GZ_EINVAL;
    return copy_to_as(&p->as, out, &card_info, sizeof(card_info));
}

bool pcmcia_node_id(uint8_t mac[6])
{
    unsigned i = 0;

    for (int n = 0; n < 64 && i + 2 < CIS_LIMIT; n++) {
        uint8_t code = CF_CIS(i);
        if (code == TPL_END)
            break;
        if (code == TPL_NULL) {
            i++;
            continue;
        }
        unsigned len = CF_CIS(i + 1);
        if (len == 0xFF || i + 2 + len > CIS_LIMIT)
            break;
        /* FUNCE, type 4 (LAN_NODE_ID), length 6, the address. */
        if (code == 0x22 && len >= 8 && CF_CIS(i + 2) == 0x04 && CF_CIS(i + 3) == 6) {
            for (int b = 0; b < 6; b++)
                mac[b] = CF_CIS(i + 4 + b);
            return true;
        }
        i += 2 + len;
    }
    return false;
}

static int cis_function(void)
{
    struct cis c;

    read_cis(&c);
    return c.function;
}

/*
 * Data moves a byte at a time, the card in 8-bit mode (set in identify).
 * On a real BE-300 after Windows CE, each 16-bit access of the data
 * register carried one byte with a zero high byte, so IDENTIFY read as
 * garbage; byte access works whatever the bridge or the card's mode.
 */
static void read_sector(uint16_t *buf)
{
    for (int i = 0; i < SECTOR / 2; i++) {
        uint16_t lo = CF_ATA(0);
        buf[i] = lo | (uint16_t)CF_ATA(0) << 8;
    }
}

static void write_sector(const uint16_t *buf)
{
    for (int i = 0; i < SECTOR / 2; i++) {
        CF_ATA(0) = (uint8_t)buf[i];
        CF_ATA(0) = (uint8_t)(buf[i] >> 8);
    }
}

/* IDENTIFY strings are big-endian within each word, space padded. */
static void identify_string(char *out, int words_at, int nwords)
{
    int n = 0;

    for (int i = 0; i < nwords; i++) {
        out[n++] = (char)(io_buf[words_at + i] >> 8);
        out[n++] = (char)io_buf[words_at + i];
    }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == 0))
        n--;
    out[n] = 0;
}

static bool identify(void)
{
    int function = cis_function();

    if (serial_card())
        return false;
    if (!socket_full || (function >= 0 && function != FUNCID_FIXED))
        return false;

    CF_ATA_CTL = CTL_SRST | CTL_NIEN;
    uint64_t t0 = time_us();
    while (time_us() - t0 < 10)
        ;
    CF_ATA_CTL = CTL_NIEN;
    if (!wait_status(ST_DRDY, ST_DRDY))
        return false;

    /* 8-bit transfers (CF spec SET FEATURES 01h); a card that refuses
     * reports an error, and the transfers below still work byte-wise. */
    CF_ATA(ATA_FEATURES) = FEATURE_8BIT;
    CF_ATA(ATA_DEVICE) = DEV_LBA;
    CF_ATA(ATA_COMMAND) = CMD_SET_FEATURES;
    (void)wait_status(0, 0);

    CF_ATA(ATA_DEVICE) = DEV_LBA;
    CF_ATA(ATA_COMMAND) = CMD_IDENTIFY;
    if (!wait_status(ST_DRQ | ST_ERR, ST_DRQ))
        return false;
    read_sector(io_buf);
    /* Words 60-61: user-addressable LBA sectors (ATA-4 8.12). */
    info.sectors = io_buf[60] | (uint32_t)io_buf[61] << 16;
    identify_string(info.model, 27, 20);
    return info.sectors != 0;
}

static void delay_ms(uint32_t ms)
{
    uint64_t t0 = time_us();
    while (time_us() - t0 < ms * 1000u)
        ;
}

/* The register state Windows CE left, and the boot card's COR when the
 * configuration window is mapped (A020 bit 3, as pcmcia.dll leaves it). */
static void note_boot_state(void)
{
    uint32_t a020 = PCMCIA_A020;

    kprintf("cf: socket CTRL %x RESET %x STATUS %x BRIDGE %x A020 %x A024 %x A034 %x\n",
            PCMCIA_CTRL, PCMCIA_RESET, PCMCIA_STATUS, PCMCIA_BRIDGE, a020, PCMCIA_A024,
            PCMCIA_A034);
    if ((a020 & 0x0Cu) == PCMCIA_A020_CONFIG) {
        boot_cor = CF_COR;
        kprintf("cf: COR %x CCSR %x\n", (unsigned)boot_cor, CF_CCSR);
    }
}

/* CE may gate the socket while no card is present. Establish the stock
 * ROM/SPL baseline before BOTH cold boot and later power-up attempts. */
static void socket_bus_init(void)
{
    REG32(0x0A00111Cu) = 1;
    REG32(0x0A00A010u) = 0x1073;
    REG32(0x0A00A014u) = 0x1061;
    REG32(0x0A00A018u) = 0x10B1;
    REG16(0x0F000140u) |= 0x20;
    REG16(0x0F000144u) |= 0x20;
}

static void socket_power_up(void)
{
    socket_bus_init();
    /* 0x0198C2F8 then 0x0198C318: Vcc, presumably. */
    PCMCIA_A034 = (PCMCIA_A034 | 0x04u) & ~0x30u;
    PCMCIA_A034 = (PCMCIA_A034 | 0x08u) & ~0x30u;
    PCMCIA_A024 = PCMCIA_A024 & ~0x0Cu;
    PCMCIA_A020 = PCMCIA_A020 & ~0x0Cu;
    PCMCIA_CTRL = 0;
    PCMCIA_RESET = PCMCIA_RESET | 1u;
    PCMCIA_CTRL = 8;
    PCMCIA_CTRL = 9;
    delay_ms(300);
    PCMCIA_RESET = PCMCIA_RESET & ~1u;
    delay_ms(20);
    PCMCIA_CTRL = 0x0B;
    PCMCIA_WINDOWS = 1;
    PCMCIA_BRIDGE = PCMCIA_BRIDGE | PCMCIA_BRIDGE_EN;
}

/* The configuration writes pcmcia.dll makes before atadisk.dll's first
 * command (traced in the emulator, the store at 0x0198A468): A034 bit 2,
 * A024 bits 2-3 clear, A020 bit 3 mapping the registers, then CCSR, COR,
 * CCSR. The COR is the boot card's; 0x43 (I/O at the secondary IDE ports,
 * level interrupts; CF spec 4.3.2) if it couldn't be read. A card still
 * settling in the socket can drop the write, so it is read back. */
static bool configure(void)
{
    /* Zero is the unconfigured memory mode; bit 7 holds the card in reset.
     * Neither is a usable inherited ATA configuration after CE had no card. */
    uint8_t cor = boot_cor >= 0 && !(boot_cor & 0x80) && (boot_cor & 0x3F)
                    ? (uint8_t)boot_cor : 0x43u;

    PCMCIA_A034 = (PCMCIA_A034 | 0x04u) & ~0x30u;
    PCMCIA_A024 = PCMCIA_A024 & ~0x0Cu;
    PCMCIA_A020 = (PCMCIA_A020 & ~0x04u) | PCMCIA_A020_CONFIG;
    for (int i = 0; i < 5; i++) {
        CF_CCSR = 0xFD;
        CF_COR = cor;
        CF_CCSR = 0x01;
        delay_ms(10);
        if (CF_COR == cor)
            return true;
        delay_ms(100);
    }
    kprintf("cf: inserted card ignores COR %x (reads %x), A020 %x A034 %x\n",
            cor, CF_COR, PCMCIA_A020, PCMCIA_A034);
    return false;
}

/*
 * DEBUG(hw): which A020 setting gives the XI-825 16-bit I/O cycles, by the
 * PRISM scratch register Linux hostap tests the same way (SWSUPPORT0 at
 * 0x28): a 16-bit write of 0x8A32 reads back whole only over a 16-bit bus.
 * Keeps the setting that works, bit 3 set if both do.
 */
#define PRISM_SWSUPPORT0    REG16(0x0A00C300u + 0x28)
static void prism_bus_width(void)
{
    uint32_t a020 = PCMCIA_A020 & ~0x0Cu;
    uint16_t with, without;

    PCMCIA_A020 = a020 | PCMCIA_A020_CONFIG;
    PRISM_SWSUPPORT0 = 0x8A32;
    with = PRISM_SWSUPPORT0;
    PCMCIA_A020 = a020;
    PRISM_SWSUPPORT0 = 0x8A32;
    without = PRISM_SWSUPPORT0;
    kprintf("cf: PRISM scratch reads %x with A020 bit 3, %x without\n", with, without);
    if (with == 0x8A32 || without != 0x8A32)
        PCMCIA_A020 = a020 | PCMCIA_A020_CONFIG;
}

/* A network card's configuration: its CONFIG register, the entry that
 * decodes NET_IO_BASE (else the default), level interrupts. */
static void configure_network(const struct cis *c)
{
    int index = c->io_index >= 0 ? c->io_index : c->index >= 0 ? c->index : 1;
    uint8_t cor = (uint8_t)(0x40u | (unsigned)index);

    if (!c->cor || c->cor >= 0x800u) {
        kprintf("cf: network card has no usable CONFIG (%x)\n", c->cor);
        return;
    }
    PCMCIA_A034 = (PCMCIA_A034 | 0x04u) & ~0x30u;
    PCMCIA_A024 = PCMCIA_A024 & ~0x0Cu;
    PCMCIA_A020 = (PCMCIA_A020 & ~0x04u) | PCMCIA_A020_CONFIG;
    REG8(0x0A00D000u + c->cor) = cor;
    delay_ms(10);
    kprintf("cf: network card %x:%x, COR %x at %x (reads %x)\n", c->manufacturer,
            c->product, cor, c->cor, REG8(0x0A00D000u + c->cor));
    if (c->manufacturer == 0xd601)
        prism_bus_width();
}

/* The tuple bytes of a card that isn't storage, for its CIS (the emulator
 * models cards from such dumps). */
static void log_cis(void)
{
    for (unsigned off = 0; off < CIS_LIMIT; off += 32) {
        kprintf("cf: cis %02x:", off);
        for (unsigned i = off; i < off + 32; i++)
            kprintf(" %02x", CF_CIS(i));
        kputs("\n");
    }
}

/* A card inserted after boot: power it, read its CIS, and bring up the
 * driver for it. Up to three power-ups: a card pushed in slowly may not
 * take the first. The emulator's cards ignore the COR. */
static void insert(void)
{
    int kind = -1;              /* the FUNCID first read: retries follow it */

    inserts++;
    for (int attempt = 0; attempt < 3; attempt++) {
        struct cis c;

        socket_power_up();
        /* Some cards (a real WL54-CF) answer only some time after reset:
         * wait for the CIS's first tuple, CISTPL_DEVICE or a link target. */
        for (uint64_t t0 = time_us(); time_us() - t0 < 1000000u;) {
            uint8_t first = CF_CIS(0);
            if (first != 0x00 && first != 0xFF)
                break;
            delay_ms(10);
        }
        read_cis(&c);
        capture_card(&c);
        if (c.function != FUNCID_FIXED && c.function != kind)
            log_cis();
        if (kind < 0)
            kind = c.function;
        if (kind == FUNCID_NETWORK) {
            if (c.function != FUNCID_NETWORK)
                continue;       /* its CIS didn't read this time */
            configure_network(&c);
            if (net_insert(c.manufacturer, c.product))
                return;
            net_removed();
            kputs("cf: network card didn't come up, powering up again\n");
            continue;
        }
        /* Serial cards remain reserved pending a verified Bluetooth match.
         * Avoid repeated resets and any ATA/network register probing. */
        if (kind == FUNCID_SERIAL) {
            if (card_info.kind == BT_CARD_SERIAL)
                kprintf("cf: serial card %x:%x has no driver\n", c.manufacturer, c.product);
            return;
        }
        /* Only storage gets ATA, whose waits stall the whole system; a card
         * whose CIS reads as nothing gets one try, the last. */
        if (kind != FUNCID_FIXED && (c.function != -1 || attempt < 2))
            continue;
        configure();
        if ((info.present = identify()))
            return;
        kprintf("cf: inserted card not ready (status %x), powering up again\n",
                CF_ATA(ATA_STATUS));
    }
}

static void probe(void)
{
    bool was = info.present, had = socket_full;

    probes++;
    socket_full = card_inserted();
    if (!socket_full) {
        if (card_info.kind != BT_CARD_ABSENT)
            forget_card();
        if (net_present()) {
            net_removed();
            kputs("cf: network card removed\n");
        }
        info.present = false;
    } else if (!had && booted) {
        insert();
    } else if (!net_present()) {
        struct cis c;
        read_cis(&c);
        capture_card(&c);
        info.present = identify();
    }
    if (info.present || was)
        info.generation++;
    if (info.present)
        kprintf("cf: %s, %u sectors\n", info.model, info.sectors);
    else if (was) {
        removals++;
        kputs("cf: card removed\n");
    }
    else if (socket_full && !net_present() && !serial_card())
        kprintf("cf: card not ready (status %x)\n", CF_ATA(ATA_STATUS));
}

/* Stock SPL 80F03248/80F03290: companion socket clock, bus timings,
 * GPIO5 direction/output. CE RAM handoff retains its live socket instead.
 * Run before net_init reads CIS: an inserted network card also needs COR. */
void pcmcia_cold_init(bool force)
{
    if (ram_boot && !(ram_boot->flags & GZ_BOOT_FROM_ROM) && !force)
        return;
    if (force)
        kputs("cf: testing cold socket initialization\n");
    socket_bus_init();
    cold_status = PCMCIA_CARD_STATUS;
    socket_full = card_inserted(); /* identify() requires the presence latch */
    if (socket_full) {
        cold_inserts++;
        insert();  /* Includes power/reset settling, CIS, COR and identify. */
    }
}

/* Live socket state for the HWDUMP=1 writer, which may have no console. */
void cf_debug(uint32_t out[8])
{
    out[0] = PCMCIA_CARD_STATUS;
    out[1] = cold_status;
    out[2] = cold_inserts;
    out[3] = inserts;
    out[4] = probes;
    out[5] = removals;
    out[6] = info.present;
    out[7] = info.generation;
}

void cf_init(void)
{
    if (ram_boot && !net_present())
        note_boot_state();
    if (!net_present() && card_inserted()) {
        PCMCIA_BRIDGE = PCMCIA_BRIDGE | PCMCIA_BRIDGE_EN;
        probe();
        if (!info.present && !net_present() && !serial_card())
            insert(); /* CE may have handed over an unpowered socket. */
        if (!info.present && !net_present() && !serial_card())
            kputs("cf: card is not an ATA disk\n");
    }
    socket_full = card_inserted();      /* a network card found at boot too */
    booted = true;
}

/* After a HIBERNATE (drivers/power.c), which cut the socket's power: what
 * was in it is gone, as if pulled, and whatever is in it now is inserted
 * afresh. The file server remounts on the new generation. */
void pcmcia_resume(void)
{
    if (net_present())
        net_removed();
    if (info.present) {
        info.present = false;
        info.generation++;
        removals++;
    }
    forget_card();
    socket_full = false;
    socket_bus_init();
    probe();
}

void pcmcia_poll(void)
{
    if (booted && card_inserted() != socket_full)
        probe();
}

static int check(struct proc *p, bool write)
{
    if (!(p->flags & (write ? PROC_FLASH : PROC_FLASH | PROC_USB)))
        return GZ_EPERM;
    return info.present ? 0 : GZ_ENOENT;
}

int sys_cf_info(struct proc *p, uint32_t uout)
{
    if (!(p->flags & (PROC_FLASH | PROC_USB)))
        return GZ_EPERM;
    /* No hot-plug interrupt: look at the socket each time, and handle an
     * insertion or removal of whatever card. */
    pcmcia_poll();
    return copy_to_as(&p->as, uout, &info, sizeof(info));
}

static int transfer(struct proc *p, uint32_t lba, uint32_t count, uint32_t ubuf, bool write)
{
    int rv = check(p, write);

    if (rv)
        return rv;
    if (count == 0 || count > MAX_SECTORS || lba >= info.sectors ||
        count > info.sectors - lba)
        return GZ_EINVAL;
    /* Move the user data before starting the command, so a bad buffer
     * never leaves the card halfway through one. */
    if (write && copy_from_as(&p->as, io_buf, ubuf, count * SECTOR))
        return GZ_EINVAL;
    if (!wait_status(ST_DRDY, ST_DRDY))
        return GZ_EIO;
    CF_ATA(ATA_COUNT) = (uint8_t)count;
    CF_ATA(ATA_LBA0) = (uint8_t)lba;
    CF_ATA(ATA_LBA1) = (uint8_t)(lba >> 8);
    CF_ATA(ATA_LBA2) = (uint8_t)(lba >> 16);
    CF_ATA(ATA_DEVICE) = DEV_LBA | ((lba >> 24) & 0x0F);
    CF_ATA(ATA_COMMAND) = write ? CMD_WRITE : CMD_READ;

    for (uint32_t i = 0; i < count; i++) {
        if (!wait_status(ST_DRQ | ST_ERR, ST_DRQ))
            return GZ_EIO;
        if (write)
            write_sector(io_buf + i * (SECTOR / 2));
        else
            read_sector(io_buf + i * (SECTOR / 2));
    }
    /* The last sector is on the card once BSY drops without an error. */
    if (!wait_status(ST_ERR | ST_DF, 0))
        return GZ_EIO;
    if (!write)
        return copy_to_as(&p->as, ubuf, io_buf, count * SECTOR);
    return 0;
}

int sys_cf_read(struct proc *p, uint32_t lba, uint32_t count, uint32_t ubuf)
{
    return transfer(p, lba, count, ubuf, false);
}

int sys_cf_write(struct proc *p, uint32_t lba, uint32_t count, uint32_t ubuf)
{
    return transfer(p, lba, count, ubuf, true);
}
