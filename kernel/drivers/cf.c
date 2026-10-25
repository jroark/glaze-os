/*
 * CompactFlash storage card: ATA in the PCMCIA card window, for the file
 * server (servers/fs), which runs FatFs on it as /Card.
 *
 * The card shares the socket with the NE2000; only one of them is ever
 * present. Transfers are polled PIO with READ/WRITE SECTORS in LBA28 mode
 * (CF+ and CompactFlash spec 6.2, ATA-4 commands 20h, 30h, ECh). The
 * emulator raises no interrupt when a command completes (be300
 * src/hw/cf.c), and polling is what the Linux port does too, so the
 * kernel never waits on GIRQ0 for the card.
 *
 * There is no hot-plug interrupt: SYS_CF_INFO samples the socket status
 * and re-identifies the card when it changes, bumping a generation count
 * the file server compares to notice a swapped card. As for the NE2000,
 * the card is assumed to decode its I/O window without a configuration
 * option register write (TODO: real cards need one, through the CIS).
 */

#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"

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
static bool socket_full;         /* the socket status at the last probe */
static bool booted;              /* cf_init has run: later insertions are hot */
static uint16_t io_buf[GZ_CF_IO_MAX / 2];

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
    return (PCMCIA_CARD_STATUS & PCMCIA_STATUS_MASK) == PCMCIA_CARD_IN;
}

/* The FUNCID tuple's function code, or -1 when the CIS has none. */
static int cis_function(void)
{
    unsigned i = 0;

    for (int n = 0; n < 64 && i < 0x400; n++) {
        uint8_t code = CF_CIS(i);
        if (code == TPL_END)
            break;
        if (code == TPL_NULL) {
            i++;
            continue;
        }
        uint8_t link = CF_CIS(i + 1);
        if (code == TPL_FUNCID)
            return CF_CIS(i + 2);
        if (link == 0xFF)
            break;
        i += 2 + link;
    }
    return -1;
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

/* A card inserted after boot is unpowered and unconfigured: power and reset
 * the socket as pcmcia.dll does on insertion (hw.h), with the PC Card
 * spec's 300 ms power-up and a settle after reset. At boot Windows CE or
 * the boot ROM has done this. Enough for the emulator; a real BE-300's card
 * still reads 0 afterwards, presumably until its configuration register is
 * set through a mapped attribute window, which isn't done yet. */
static void socket_power_up(void)
{
    PCMCIA_A020 = 0;
    PCMCIA_A024 = 0;
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

static void probe(void)
{
    bool was = info.present, had = socket_full;

    socket_full = card_inserted();
    if (socket_full && !had && booted)
        socket_power_up();
    info.present = identify();
    if (info.present || was)
        info.generation++;
    if (info.present)
        kprintf("cf: %s, %u sectors\n", info.model, info.sectors);
    else if (was)
        kputs("cf: card removed\n");
    else if (socket_full)
        kprintf("cf: card not ready (status %x)\n", CF_ATA(ATA_STATUS));
}

void cf_init(void)
{
    if (!ne2000_present() && card_inserted()) {
        PCMCIA_BRIDGE = PCMCIA_BRIDGE | PCMCIA_BRIDGE_EN;
        probe();
        if (!info.present)
            kputs("cf: card is not an ATA disk\n");
    }
    booted = true;
}

static int check(struct proc *p)
{
    if (!(p->flags & PROC_FLASH))
        return GZ_EPERM;
    return info.present ? 0 : GZ_ENOENT;
}

int sys_cf_info(struct proc *p, uint32_t uout)
{
    if (!(p->flags & PROC_FLASH))
        return GZ_EPERM;
    /* No hot-plug interrupt: look at the socket each time and re-identify
     * on a change. An NE2000 owns the socket for the whole boot. */
    if (!ne2000_present() && card_inserted() != socket_full)
        probe();
    return copy_to_as(&p->as, uout, &info, sizeof(info));
}

static int transfer(struct proc *p, uint32_t lba, uint32_t count, uint32_t ubuf, bool write)
{
    int rv = check(p);

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
