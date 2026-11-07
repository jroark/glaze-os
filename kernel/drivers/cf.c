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
#define FEATURE_16BIT   0x81    /* CF spec: disable 8-bit transfers */

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
static uint64_t cis_retry_at;
static unsigned cis_retries;

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

/*
 * Two sockets (hw.h): the CF socket (0) and the PCMCIA jacket's (1), which
 * Windows CE drives through a second controller. The controller reads 0
 * while its bus is off, so it is only checked with the bus on; a card in
 * the jacket is otherwise seen by two companion signals (CE, card in and
 * out: 0x0A001008 bit 3 and 0x0A00104C bit 6 both clear with a card).
 * Glaze takes over a card CE left powered and configured (a RAM boot), or
 * powers the socket itself (socket1_power_up). docs/hardware/PCCARDS.md.
 */
uint32_t pcmcia_io_base = PCMCIA0_IO, pcmcia_attr_base = PCMCIA0_ATTR;
static unsigned socket;         /* the socket the windows point at */
static bool socket1_dead;       /* its power-up failed: wait for a new card */
static bool socket1_ours;       /* Glaze powered it, so the COR is Glaze's */

static bool socket1_card(void)
{
    return !(PCMCIA1_DETECT & PCMCIA1_EMPTY) && !(PCMCIA1_PRESENCE & PCMCIA1_ABSENT);
}

static bool socket1_up(void)
{
    if (!socket1_card() || !(PCMCIA1_BUS & PCMCIA1_BUS_ON))
        return false;
    return PCMCIA1_ID == PCMCIA1_ID_VALUE && PCMCIA1_CHIP == PCMCIA1_CHIP_ID &&
           (PCMCIA1_CTRL & PCMCIA1_CTRL_UP) == PCMCIA1_CTRL_UP;
}

static void use_socket(unsigned s)
{
    socket = s;
    pcmcia_io_base = s ? PCMCIA1_IO : PCMCIA0_IO;
    pcmcia_attr_base = s ? PCMCIA1_ATTR : PCMCIA0_ATTR;
}

/* Whether a card is in either socket, pointing the windows at it. A card
 * up in the jacket comes first, then the CF socket's own card detect, then
 * a jacket card still to be powered. */
bool pcmcia_card_present(void)
{
    if (!socket1_card())
        socket1_dead = false;       /* emptied: the next card gets a try */
    if (socket1_up()) {
        use_socket(1);
        return true;
    }
    if (PCMCIA0_CARD_PRESENT()) {
        use_socket(0);
        return true;
    }
    if (socket1_card() && !socket1_dead) {
        use_socket(1);
        return true;
    }
    use_socket(0);
    return false;
}

static bool card_inserted(void)
{
    return pcmcia_card_present();
}

/* GIRQ0 source 6, socket 1's interrupt (hw.h): clear the socket's cause
 * and any other jacket events, as the OAL does before the card's ISR runs,
 * so the source can be enabled again. Called from the interrupt handler. */
void pcmcia1_irq_ack(void)
{
    uint32_t other = PCMCIA1_EVENTS & PCMCIA1_EVENT_OTHER;
    if (other)
        PCMCIA1_EVENT_ACK = other;
    if (socket != 1 || !(PCMCIA1_BUS & PCMCIA1_BUS_ON))
        return;
    if (PCMCIA1_IRQ_SUMMARY & PCMCIA1_IRQ_SOCKET)
        (void)PCMCIA1_CAUSE;
}

/* From the tick: with a driver running for the jacket's card, route its
 * interrupt to GIRQ0 source 6 as the OAL does (0x8007B088, 0x800A9420):
 * only the socket's interrupt unmasked at 0x0A001B58, the socket's
 * enable at 0x0B000208, then the source. The handler masks the source
 * while it runs and enables it again (irq.c). */
static bool socket1_irq_armed;
void pcmcia1_irq_arm(bool driver_up)
{
    if (socket != 1 || !driver_up || !(PCMCIA1_BUS & PCMCIA1_BUS_ON)) {
        socket1_irq_armed = false;
        return;
    }
    if (socket1_irq_armed)
        return;
    socket1_irq_armed = true;
    PCMCIA1_EVENT_ACK = PCMCIA1_EVENT_OTHER;
    PCMCIA1_EVENT_MASK = PCMCIA1_EVENT_IRQ;
    PCMCIA1_IRQ_ENABLE = 1;
    (void)PCMCIA1_CAUSE;
    KJ_SRC_JACKET = 0x30u;              /* clear its status, as the OAL does */
    KJ_SRC_JACKET = KJ_SRC_MODE | KJ_SRC_ENABLE;
    kputs("cf: socket 1 interrupt on GIRQ0 source 6\n");
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
    bool readable;                      /* at least one recognized identity tuple */
    int function;                       /* -1: no FUNCID */
    unsigned manufacturer, product;
    uint32_t cor;                       /* attribute address, 0: none */
    int index, io_index;                /* default entry; one decoding 0x300 */
    int uart_index;                     /* an entry decoding UART_IO_BASE */
    bool modem;                         /* serial FUNCE names a modem function */
};

/* A CFTABLE_ENTRY's index and whether it can decode NET_IO_BASE: a range
 * starting there, or address lines only (the window picks the base). With
 * verbose, the entry's I/O ranges and IRQ are logged, so an unknown card
 * can be configured from one boot log (PC Card spec "CFTABLE_ENTRY"). */
static void cis_entry(unsigned at, unsigned len, struct cis *c, bool verbose)
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
    if (verbose)
        kprintf("cf: cftable %u%s:", index, indx & 0x40 ? " (default)" : "");
    if (!(fs & 0x08)) {
        if (verbose)
            kputs(" no I/O\n");
        return;
    }
    uint8_t io = NEXT();
    bool decodes = !(io & 0x80) && (io & 0x1F) >= 5;
    if (verbose)
        kprintf(" %u lines%s%s", io & 0x1F, io & 0x20 ? " 8-bit" : "", io & 0x40 ? " 16-bit" : "");
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
            uint32_t length = 0;
            for (unsigned b = 0; b < lsz; b++)
                length |= (uint32_t)NEXT() << (8 * b);
            decodes = decodes || base == NET_IO_BASE;
            uart = uart || base == UART_IO_BASE;
            if (verbose)
                kprintf(" %x+%x", base, length + 1);
        }
    }
    if (verbose) {
        if (fs & 0x10) {
            uint8_t irq = NEXT();
            kprintf(" irq %x%s", irq, irq & 0x20 ? " level" : "");
        }
        kputs("\n");
    }
    if (decodes && c->io_index < 0)
        c->io_index = index;
    if (uart && c->uart_index < 0)
        c->uart_index = index;
#undef NEXT
}

static void read_cis(struct cis *c, bool verbose)
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
        if (code == 0x01 && len >= 1) c->readable = true; /* CISTPL_DEVICE */
        if (code == 0x15 && len >= 2 && CF_CIS(at) >= 4 && CF_CIS(at) <= 5)
            c->readable = true;          /* CISTPL_VERS_1 */
        if (code == TPL_FUNCID && len >= 1) {
            c->function = CF_CIS(at);
            if (c->function <= 8) c->readable = true;
        } else if (code == TPL_MANFID && len >= 4) {
            c->manufacturer = CF_CIS(at) | (unsigned)CF_CIS(at + 1) << 8;
            c->product = CF_CIS(at + 2) | (unsigned)CF_CIS(at + 3) << 8;
        } else if (code == TPL_CONFIG && len >= 3) {
            unsigned rasz = (CF_CIS(at) & 3) + 1;
            c->cor = 0;
            for (unsigned b = 0; b < rasz && 2 + b < len; b++)
                c->cor |= (uint32_t)CF_CIS(at + 2 + b) << (8 * b);
        } else if (code == TPL_CFTABLE && len >= 1) {
            cis_entry(at, len, c, verbose);
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
    /* Any other serial card with a COM1 entry: GPS receivers (NMEA CF cards
     * such as the AmbiCom GPS-CF, docs/hardware/GPS.md) are plain 16550s
     * with no modem FUNCE. gps.c only listens until it hears NMEA. */
    else if (c->function == FUNCID_SERIAL && c->cor && c->uart_index >= 0)
        card_info.kind = BT_CARD_GPS;
    if (card_info.kind == BT_CARD_BLUETOOTH) bt_insert();
    if (card_info.kind == BT_CARD_MODEM) modem_insert(c->cor, (unsigned)c->uart_index);
    if (card_info.kind == BT_CARD_GPS) gps_insert(c->cor, (unsigned)c->uart_index);
}

/* A serial card owns the socket: never ATA-probe or reset it. */
static bool serial_card(void)
{
    return card_info.kind == BT_CARD_SERIAL || card_info.kind == BT_CARD_BLUETOOTH ||
           card_info.kind == BT_CARD_MODEM || card_info.kind == BT_CARD_GPS;
}

static void forget_card(void)
{
    cis_retry_at = 0; cis_retries = 0;
    bt_removed();
    modem_removed();
    gps_removed();
    uint32_t generation = card_info.generation + 1;
    memset(&card_info, 0, sizeof(card_info));
    card_info.version = BT_CIS_VERSION;
    card_info.generation = generation;
}

/* Veo Photo Traveler, captured on a BE-300 2026-11-04 (docs/hardware/VEO.md):
 * MANFID ffff:0001, FUNCID fe, VERS_1 "VEO" "CFTraveler622". */
bool cf_card_is_veo(void)
{
    return card_info.kind == BT_CARD_OTHER && card_info.function == 0xfe &&
           card_info.manufacturer == 0xffff && card_info.product == 0x0001 &&
           strstr(card_info.identification, "VEO") &&
           strstr(card_info.identification, "CFTraveler622");
}

/* The Ambicom DGCAM-CF camera's 8 MB FAT16 store, an ICSI IC3100 ATA
 * controller that transfers data only in 16-bit words (read_sector). */
bool cf_card_is_icsi_camera(void)
{
    return card_info.function == FUNCID_FIXED && card_info.manufacturer == 0 &&
           card_info.product == 0 && strstr(card_info.identification, "ICSI EMULATION");
}

/* Once a USB probe has touched the ICSI card, stop sending it IDENTIFY. */
static bool probe_hold;
void cf_probe_hold(void) { probe_hold = true; }

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

    read_cis(&c, false);
    return c.function;
}

/*
 * Data normally moves a byte at a time, the card in 8-bit mode (set in
 * identify). On a real BE-300 after Windows CE, each 16-bit access of the
 * data register carried one byte with a zero high byte, so IDENTIFY read
 * as garbage. The Ambicom DGCAM-CF's ICSI IC3100 controller is the
 * opposite: it aborts SET FEATURES 01h, byte reads of its data register
 * return zeros, and 16-bit reads return the data (identify matches it).
 */
static bool wide_data;

static void read_sector(uint16_t *buf)
{
    if (wide_data) {
        for (int i = 0; i < SECTOR / 2; i++)
            buf[i] = CF_ATA_DATA16;
        return;
    }
    for (int i = 0; i < SECTOR / 2; i++) {
        uint16_t lo = CF_ATA(0);
        buf[i] = lo | (uint16_t)CF_ATA(0) << 8;
    }
}

static void write_sector(const uint16_t *buf)
{
    if (wide_data) {
        for (int i = 0; i < SECTOR / 2; i++)
            CF_ATA_DATA16 = buf[i];
        return;
    }
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

/* OnSpec 02b6:0003 (VERS_1 1.06), captured on a BE-300: IDENTIFY
 * presents each byte in the low lane of a word even though READ SECTORS
 * obeys SET FEATURES 01h. ATA-4 8.12 still defines the resulting words;
 * normalize this adapter's transport before interpreting them. */
static bool identify_low_lane(unsigned manufacturer, unsigned product)
{
    if (manufacturer != 0x02b6 || product != 0x0003 ||
        io_buf[0] != 0x8a || io_buf[1] != 0x84)
        return false;
    for (unsigned i = 0; i < 256; ++i)
        if (io_buf[i] & 0xff00) return false;
    return true;
}

static void identify_pack_low_lane(void)
{
    /* The adapter completes the command after the usual 512 bus bytes:
     * only ATA words 0..127 are available, including all LBA28 fields.
     * Do not invent values for the unavailable optional upper words. */
    for (unsigned i = 0; i < 128; ++i)
        io_buf[i] = io_buf[2*i] | (uint16_t)io_buf[2*i+1] << 8;
    memset(io_buf + 128, 0, 128 * sizeof(io_buf[0]));
}

/* Reset the card, choose the data width (CF spec SET FEATURES 01h enables
 * 8-bit transfers, 81h disables them; a card that refuses reports an error)
 * and read IDENTIFY into io_buf. */
static bool identify_as(bool wide)
{
    CF_ATA_CTL = CTL_SRST | CTL_NIEN;
    uint64_t t0 = time_us();
    while (time_us() - t0 < 10)
        ;
    CF_ATA_CTL = CTL_NIEN;
    if (!wait_status(ST_DRDY, ST_DRDY))
        return false;

    /* The IC3100 aborts both 01h and 81h; it has no 8-bit mode. */
    CF_ATA(ATA_FEATURES) = wide ? FEATURE_16BIT : FEATURE_8BIT;
    CF_ATA(ATA_DEVICE) = DEV_LBA;
    CF_ATA(ATA_COMMAND) = CMD_SET_FEATURES;
    (void)wait_status(0, 0);

    wide_data = wide;
    CF_ATA(ATA_DEVICE) = DEV_LBA;
    CF_ATA(ATA_COMMAND) = CMD_IDENTIFY;
    if (!wait_status(ST_DRQ | ST_ERR, ST_DRQ))
        return false;
    read_sector(io_buf);
    if (identify_low_lane(card_info.manufacturer, card_info.product)) {
        identify_pack_low_lane();
        kputs("cf: normalized OnSpec IDENTIFY low-byte lanes\n");
    }
    /* Words 60-61: user-addressable LBA sectors (ATA-4 8.12). */
    info.sectors = io_buf[60] | (uint32_t)io_buf[61] << 16;
    identify_string(info.model, 27, 20);
    return info.sectors != 0;
}

static bool identify(void)
{
    int function = cis_function();

    if (serial_card())
        return false;
    if (probe_hold && cf_card_is_icsi_camera())
        return false;
    if (!socket_full || (function >= 0 && function != FUNCID_FIXED))
        return false;

    /* The IC3100 (see read_sector) is matched by its CIS rather than
     * probed: after a byte-wide IDENTIFY leaves its data unread, a soft
     * reset hangs it busy (status 80) until the COR's SRESET. */
    bool wide = cf_card_is_icsi_camera();
    if (identify_as(wide)) {
        if (wide)
            kprintf("cf: %s: 16-bit data transfers\n", info.model);
        return true;
    }
    wide_data = false;
    return false;
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
    if ((a020 & 0x0Cu) == PCMCIA_A020_CONFIG && PCMCIA0_CARD_PRESENT() && !net_present()) {
        boot_cor = CF_COR;
        kprintf("cf: COR %x CCSR %x\n", (unsigned)boot_cor, CF_CCSR);
    }
    /* The jacket's socket: its controller is read only while powered. */
    uint32_t detect = PCMCIA1_DETECT, bus = PCMCIA1_BUS;
    if (bus & PCMCIA1_BUS_ON)
        kprintf("cf: socket 1 detect %x bus %x id %x/%x CTRL %x STATUS %x A520 %x\n", detect, bus,
                PCMCIA1_ID, PCMCIA1_CHIP, PCMCIA1_CTRL, PCMCIA1_STATUS, PCMCIA1_A520);
    else
        kprintf("cf: socket 1 detect %x bus %x (off)\n", detect, bus);
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

/*
 * Socket 1's power-up, from pcmcia.dll (be300-modules 74_pcmcia.dll, base
 * 0x01980000), whose socket 1 helpers address these registers:
 * - 0x0198C83C: GPIO 5 on (0x0198C784, then 30 ms), 0x0A001B04 = 0,
 *   0x0A001B00 bit 0; 0x0198B46C: up to 10 x 2 ms for the socket's interrupt line
 *   (0x0A001B50 bit 3), 0x0B00011C = 1, then 0x0B000140 = 1, 30 ms,
 *   0x0B000130 = 1, 50 ms, and timings by 0x0B000508 bit 8;
 * - 0x0198CDDC: 0x0B000128 = 1; 0x0198ADA8: 0x0B00014C = 0, 554 = 1,
 *   510 = 0x11, 514/518 = 0x1062, 540-548 = 0, 500 bit 4 clear, 504 bit 2;
 *   0x0A00104C bits 4-5; 500 bit 0 clear; 0x0198B854 then 0x0198B7B4: the
 *   pin detectors 108-118 and 150 rearmed (0x30, then 3), 204 = 1, 208 = 1;
 * - the card: 524 and 520 bits 2-3 clear, then CTRL bit 3, reset (504 bit
 *   0), CTRL bit 0, reset released, CTRL bit 1 (0xB), as on the CF socket
 *   (socket_power_up), and window 0 (51C bit 2), as CE leaves them.
 * pcmcia.dll's control and reset helpers (0x0198B230-0x0198B3D4) are never
 * called directly, so the card order follows the CF socket's traced one
 * and the register values CE left with an NE2000 up (2026-11-05).
 */
/* The highest attribute address a COR can be written at: below the CF
 * socket's NAND port (hw.h CF_CIS), anywhere in socket 1's 2 MiB window. */
static uint32_t cor_limit(void) { return socket == 1 ? 0x200000u : 0x800u; }

#define S1(off)         REG32(0x0B000000u + (off))
#define COMPANION(off)  REG32(0x0A001000u + (off))
static bool socket1_power_up(void)
{
    socket_bus_init();                  /* GPIO 5, as 0x0198C784 */
    delay_ms(30);
    COMPANION(0xB04) = 0;
    COMPANION(0xB00) |= 1;
    for (int i = 0; i < 50 && !(COMPANION(0xB50) & 8); i++)
        delay_ms(2);
    S1(0x11C) = 1;
    S1(0x140) = 1;
    delay_ms(30);
    S1(0x130) = 1;
    delay_ms(50);
    if (PCMCIA1_ID != PCMCIA1_ID_VALUE || PCMCIA1_CHIP != PCMCIA1_CHIP_ID) {
        kprintf("cf: socket 1 controller not answering (power %x, id %x/%x)\n",
                COMPANION(0xB50), PCMCIA1_ID, PCMCIA1_CHIP);
        return false;
    }
    if (S1(0x508) & 0x100) {
        S1(0x514) = S1(0x518) = 0x1031;
        S1(0x144) = 0;
        S1(0x134) = 1;
    } else {
        S1(0x514) = S1(0x518) = 0x1062;
        S1(0x134) = 0;
        S1(0x144) = 1;
    }
    S1(0x128) = 1;
    S1(0x14C) = 0;
    S1(0x554) = 1;
    S1(0x510) = 0x11;
    S1(0x514) = S1(0x518) = 0x1062;
    S1(0x540) = S1(0x544) = S1(0x548) = 0;
    S1(0x500) &= ~0x10u;
    S1(0x504) |= 4;
    COMPANION(0x4C) |= 0x30;
    S1(0x500) &= ~1u;
    S1(0x204) = 0;
    static const unsigned pins[] = {0x108, 0x10C, 0x110, 0x150, 0x114, 0x118};
    for (unsigned i = 0; i < 6; i++)
        S1(pins[i]) = 0x30;
    S1(0x208) = 1;
    for (unsigned i = 0; i < 6; i++)
        S1(pins[i]) = 3;
    S1(0x204) = 1;
    /* The card's supply and reset. */
    S1(0x524) &= ~0x0Cu;
    S1(0x520) &= ~0x0Cu;
    S1(0x504) |= 1;
    S1(0x500) |= 8;
    S1(0x500) |= 1;
    delay_ms(300);
    S1(0x504) &= ~1u;
    delay_ms(20);
    S1(0x500) |= 2;
    S1(0x51C) |= 4;
    S1(0x520) = (S1(0x520) & ~4u) | 8;  /* attribute memory and the COR */
    kprintf("cf: socket 1 powered: CTRL %x RESET %x STATUS %x A520 %x\n", S1(0x500), S1(0x504),
            S1(0x508), S1(0x520));
    socket1_ours = true;
    return true;
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
 * DEBUG(hw): which A020 setting gives a network card 16-bit I/O cycles, by
 * a scratch register a 16-bit write of 0x8A32 reads back whole from only
 * over a 16-bit bus: the PRISM's SWSUPPORT0 at 0x28 (Linux hostap tests it
 * the same way), or a 3c589's window 2 station-address word at 0 (window 2
 * selected through the command register at 0x0E, EtherLink III reference
 * "Register Windows"; el3_init rewrites the address afterwards).
 * Keeps the setting that works, bit 3 set if both do.
 */
#define NET_REG16(r)    REG16(0x0A00C000u + NET_IO_BASE + (r))
static void bus_width(const char *what, unsigned reg)
{
    uint32_t a020 = PCMCIA_A020 & ~0x0Cu;
    uint16_t with, without;

    PCMCIA_A020 = a020 | PCMCIA_A020_CONFIG;
    NET_REG16(reg) = 0x8A32;
    with = NET_REG16(reg);
    PCMCIA_A020 = a020;
    NET_REG16(reg) = 0x8A32;
    without = NET_REG16(reg);
    kprintf("cf: %s scratch reads %x with A020 bit 3, %x without\n", what, with, without);
    if (with == 0x8A32 || without != 0x8A32)
        PCMCIA_A020 = a020 | PCMCIA_A020_CONFIG;
}

/* A network card's configuration: its CONFIG register, the entry that
 * decodes NET_IO_BASE (else the default), level interrupts. */
static void configure_network(const struct cis *c)
{
    int index = c->io_index >= 0 ? c->io_index : c->index >= 0 ? c->index : 1;
    uint8_t cor = (uint8_t)(0x40u | (unsigned)index);

    if (!c->cor || c->cor >= cor_limit()) {
        kprintf("cf: network card has no usable CONFIG (%x)\n", c->cor);
        return;
    }
    if (socket == 1) {
        /* Configured by Windows CE before a RAM boot: keep its COR. Else
         * socket1_power_up left the configuration window mapped. */
        if (socket1_ours) {
            REG8(pcmcia_attr_base + c->cor) = cor;
            delay_ms(10);
        }
        kprintf("cf: jacket network card %x:%x, COR %x at %x%s\n", c->manufacturer, c->product,
                REG8(pcmcia_attr_base + c->cor), c->cor, socket1_ours ? "" : " as CE left it");
        return;
    }
    PCMCIA_A034 = (PCMCIA_A034 | 0x04u) & ~0x30u;
    PCMCIA_A024 = PCMCIA_A024 & ~0x0Cu;
    PCMCIA_A020 = (PCMCIA_A020 & ~0x04u) | PCMCIA_A020_CONFIG;
    REG8(0x0A00D000u + c->cor) = cor;
    delay_ms(10);
    kprintf("cf: network card %x:%x, COR %x at %x (reads %x)\n", c->manufacturer,
            c->product, cor, c->cor, REG8(0x0A00D000u + c->cor));
    if (c->manufacturer == 0xd601 || c->manufacturer == 0x0156) {
        bus_width("HFA384x", 0x28);     /* PRISM and Hermes SWSUPPORT0 */
    } else if (c->manufacturer == 0x0101) {
        NET_REG16(0x0E) = 0x0802;           /* SelectWindow 2 */
        bus_width("3c589", 0);
    }
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
    struct cis c;
    read_cis(&c, true);
    kprintf("cf: card %x:%x function %d config %x \"%s\"\n", c.manufacturer, c.product,
            c.function, c.cor, card_info.identification);
}

/* A card inserted after boot: power it, read its CIS, and bring up the
 * driver for it. Up to three power-ups: a card pushed in slowly may not
 * take the first. The emulator's cards ignore the COR. */
/* A card Windows CE left up in the jacket (socket 1): its CIS and driver,
 * without touching the socket. Only network and serial cards. */
static void insert_jacket(void)
{
    struct cis c;

    /* Taking over CE's live socket is for boot; a hot insertion is always
     * powered and reset, even if a quick swap left the socket up. */
    if (booted || !socket1_up()) {
        socket1_ours = false;
        if (!socket1_power_up()) {
            socket1_dead = true;
            return;
        }
        for (uint64_t t0 = time_us(); time_us() - t0 < 1000000u;) {
            uint8_t first = CF_CIS(0);
            if (first != 0x00 && first != 0xFF)
                break;
            delay_ms(10);
        }
    }
    read_cis(&c, false);
    capture_card(&c);           /* modems go to modem_insert here */
    log_cis();
    if (c.function == FUNCID_NETWORK) {
        configure_network(&c);
        if (!net_insert(c.manufacturer, c.product))
            kputs("cf: jacket network card didn't come up\n");
    } else if (card_info.kind == BT_CARD_SERIAL) {
        kprintf("cf: serial card %x:%x has no driver\n", c.manufacturer, c.product);
    } else if (c.function != FUNCID_SERIAL) {
        kprintf("cf: jacket card function %d not supported\n", c.function);
    }
}

static void insert(bool power_up)
{
    int kind = -1;              /* the FUNCID first read: retries follow it */
    bool readable = false;

    cis_retry_at = 0;
    inserts++;
    if (socket == 1) {
        insert_jacket();
        return;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        struct cis c;

        /* Deferred CIS reads must let the card finish starting. Repeating
         * the reset here can keep a slow card permanently unreadable. */
        if (power_up || attempt) socket_power_up();
        /* CE's attribute/configuration access helper 0198BF98. */
        PCMCIA_A020 = (PCMCIA_A020 & ~0x04u) | PCMCIA_A020_CONFIG;
        /* Some cards (a real WL54-CF) answer only some time after reset:
         * wait for the CIS's first tuple, CISTPL_DEVICE or a link target. */
        for (uint64_t t0 = time_us(); (power_up || attempt) && time_us() - t0 < 1000000u;) {
            uint8_t first = CF_CIS(0);
            if (first != 0x00 && first != 0xFF)
                break;
            delay_ms(10);
        }
        read_cis(&c, false);
        capture_card(&c);
        /* A nonzero bus byte is not evidence of a readable CIS. The card
         * may still be settling; never configure an unknown serial card
         * as ATA based on stale/open-bus bytes. PC Card CIS tuple codes. */
        if (!c.readable) {
            kprintf("cf: unreadable CIS (%x), retrying\n", CF_CIS(0));
            break; /* Yield to USB/UI before the deferred attempt. */
        }
        readable = true;
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
    if (!readable && cis_retries && card_inserted()) {
        cis_retries--;
        cis_retry_at = time_us() + 500000u;
    }
}

static void probe(void)
{
    bool was = info.present, had = socket_full;

    probes++;
    socket_full = card_inserted();
    if (!socket_full) {
        /* A card pulled from the jacket: cut its supply (control bits 0, 1
         * and 3, set by socket1_power_up), so the next one is powered and
         * reset afresh rather than taken over. */
        if ((PCMCIA1_BUS & PCMCIA1_BUS_ON) && (PCMCIA1_CTRL & PCMCIA1_CTRL_UP)) {
            PCMCIA1_CTRL &= ~PCMCIA1_CTRL_UP;
            kputs("cf: socket 1 card powered off\n");
        }
        if (card_info.kind != BT_CARD_ABSENT)
            forget_card();
        if (net_present()) {
            net_removed();
            kputs("cf: network card removed\n");
        }
        info.present = false;
    } else if (!had && booted) {
        cis_retries = 20;
        insert(true);
    } else if (!net_present()) {
        struct cis c;
        read_cis(&c, false);
        capture_card(&c);
        /* ATA is at the CF socket's ports only; a jacket card is handled
         * by insert_jacket. */
        info.present = socket == 0 && c.readable && identify();
    }
    if (info.present || was)
        info.generation++;
    if (info.present)
        kprintf("cf: %s, %u sectors\n", info.model, info.sectors);
    else if (was) {
        removals++;
        kputs("cf: card removed\n");
    }
    else if (socket == 0 && socket_full && !net_present() && !serial_card())
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
        insert(true);  /* Includes power/reset settling, CIS, COR and identify. */
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
    cis_retries = 20;
    if (ram_boot)
        note_boot_state();
    if (!net_present() && card_inserted()) {
        if (socket == 0)
            PCMCIA_BRIDGE = PCMCIA_BRIDGE | PCMCIA_BRIDGE_EN;
        probe();
        if (!info.present && !net_present() && !serial_card())
            insert(true); /* CE may have handed over an unpowered socket. */
        if (!info.present && !net_present() && !serial_card())
            kputs("cf: card is not an ATA disk\n");
    }
    socket_full = card_inserted();      /* a network card found at boot too */
    booted = true;
}

/* Before a sleep (drivers/power.c): a network card, a radio especially,
 * draws far more than the suspended CPU, and SUSPEND leaves the socket
 * powered. The card goes as if pulled and its socket loses power;
 * pcmcia_resume() inserts it afresh at the wake, and the network server
 * attaches and reconnects. Storage, modem and Bluetooth cards stay up.
 * docs/hardware/POWER.md "Network cards". */
bool pcmcia_suspend(void)
{
    if (!net_present())
        return false;
    net_removed();
    if (socket == 1) {
        /* As probe() does for a card pulled from the jacket. */
        if (PCMCIA1_BUS & PCMCIA1_BUS_ON)
            PCMCIA1_CTRL &= ~PCMCIA1_CTRL_UP;
    } else {
        /* pcmcia.dll's supply helpers (be300-modules 74_pcmcia.dll):
         * 0x0198C2D8 sets A034 bit 2 for Vcc on and clears it (with bits 4
         * and 5) for off; 0x0198C31C sets bit 3; 0x0198C33C clears bits 2
         * and 3 together. socket_power_up() sets both. Stop decoding the
         * card's windows first. */
        PCMCIA_BRIDGE &= ~PCMCIA_BRIDGE_EN;
        PCMCIA_A034 &= ~0x0Cu;
    }
    kprintf("cf: network card in socket %u powered off for the sleep\n", socket);
    return true;
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
    else if (booted && socket_full && cis_retry_at && time_us() >= cis_retry_at) {
        insert(false);
        /* A delayed storage identification is an insertion too: the file
         * server may already have observed the earlier absent generation. */
        if (info.present) info.generation++;
    }
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
