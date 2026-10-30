/*
 * NE2000 PCMCIA Ethernet (a DP8390 core with 16 KiB of card RAM).
 *
 * The kernel only moves frames: the interrupt handler copies received
 * frames out of the card's ring into a small queue and notifies the bound
 * port, and SYS_NET_SEND transmits one frame from the card's transmit
 * buffer. The TCP/IP stack is the network server (servers/net), the one
 * process the kernel starts with PROC_NET.
 *
 * Register use follows the DP8390D datasheet (the "NIC Core" register map,
 * remote DMA and buffer ring chapters) and the emulator's model,
 * be300 src/hw/cf_ne2000.c. The card is assumed to be in I/O mode at port
 * 0x300 once the bridge windows are on, as on the emulator and in the
 * Linux port; real cards would need their configuration option register
 * written through attribute memory first (TODO: not done, no CIS parser).
 */

#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"

/* Page 0 registers (page 1: PAR0..5 at 1..6, CURR at 7, MAR0..7 at 8..15). */
#define CR      0x00
#define PSTART  0x01
#define PSTOP   0x02
#define BNRY    0x03
#define TPSR    0x04    /* write; TSR on read */
#define TBCR0   0x05
#define TBCR1   0x06
#define ISR     0x07
#define RSAR0   0x08
#define RSAR1   0x09
#define RBCR0   0x0A
#define RBCR1   0x0B
#define RCR     0x0C
#define TCR     0x0D
#define DCR     0x0E
#define IMR     0x0F
#define PAR0    0x01
#define CURR    0x07
#define MAR0    0x08

#define CR_STP      0x01
#define CR_STA      0x02
#define CR_TXP      0x04
#define CR_RREAD    0x08
#define CR_RWRITE   0x10
#define CR_NODMA    0x20
#define CR_PAGE1    0x40

#define ISR_PRX     0x01
#define ISR_PTX     0x02
#define ISR_RXE     0x04
#define ISR_TXE     0x08
#define ISR_OVW     0x10
#define ISR_CNT     0x20
#define ISR_RDC     0x40
#define ISR_RST     0x80

#define DCR_WTS     0x01    /* word-wide remote DMA */
#define NE_DATA_PORT 0x10
#define DCR_LS      0x08    /* normal operation, not loopback */
#define DCR_FT1     0x40    /* FIFO threshold 8 bytes */
#define RCR_AB      0x04    /* broadcast */
#define TCR_LB0     0x02    /* internal loopback */

/* Card RAM pages (256 bytes each) 0x40..0x7F: six for transmit, the rest
 * for the receive ring. */
#define TX_PAGE     0x40
#define RX_START    0x46
#define RX_STOP     0x80

static bool present;
static uint8_t mac[6];
static uint8_t rx_buf[1536];
static uint8_t run = CR_STP;        /* CR_STA once the NIC is started */
/* Word-wide remote DMA (DCR_WTS), or byte-wide: Windows CE's own NE2000
 * driver reads the data port a byte at a time on a BE-300, and a card whose
 * PROM doesn't read word-wide is tried again byte-wide (DP8390D "Data
 * Configuration Register"; Linux ne.c probes the same way). */
static bool wide = true;

static void page0(void)
{
    NE_REG(CR) = CR_NODMA | run;
}

static bool wait_isr(uint8_t bits)
{
    for (int i = 0; i < 100000; i++) {
        if (NE_REG(ISR) & bits)
            return true;
    }
    return false;
}

/* Remote DMA moves 16-bit words (odd lengths transfer one extra byte), or
 * bytes when !wide. It is always issued with STA, as Linux ne.c does even
 * while probing in loopback: a real DP8390 runs no remote DMA stopped, which
 * the emulator doesn't model. */
static void dma_read(uint16_t addr, void *dst, uint16_t len)
{
    uint8_t *d = dst;
    uint16_t n = wide ? (uint16_t)((len + 1) & ~1) : len;

    page0();
    NE_REG(RBCR0) = (uint8_t)n;
    NE_REG(RBCR1) = (uint8_t)(n >> 8);
    NE_REG(RSAR0) = (uint8_t)addr;
    NE_REG(RSAR1) = (uint8_t)(addr >> 8);
    NE_REG(CR) = CR_RREAD | CR_STA;
    if (!wide) {
        for (uint16_t i = 0; i < n; i++)
            d[i] = NE_REG(NE_DATA_PORT);
    } else {
        for (uint16_t i = 0; i < n; i += 2) {
            uint16_t w = NE_DATA16;
            d[i] = (uint8_t)w;
            if (i + 1 < len)
                d[i + 1] = (uint8_t)(w >> 8);
        }
    }
    wait_isr(ISR_RDC);
    NE_REG(ISR) = ISR_RDC;
}

static void dma_write(uint16_t addr, const uint8_t *src, uint16_t len)
{
    uint16_t n = wide ? (uint16_t)((len + 1) & ~1) : len;

    page0();
    NE_REG(ISR) = ISR_RDC;
    NE_REG(RBCR0) = (uint8_t)n;
    NE_REG(RBCR1) = (uint8_t)(n >> 8);
    NE_REG(RSAR0) = (uint8_t)addr;
    NE_REG(RSAR1) = (uint8_t)(addr >> 8);
    NE_REG(CR) = CR_RWRITE | CR_STA;
    if (!wide) {
        for (uint16_t i = 0; i < n; i++)
            NE_REG(NE_DATA_PORT) = src[i];
    } else {
        for (uint16_t i = 0; i < n; i += 2)
            NE_DATA16 = (uint16_t)(src[i] | (i + 1 < len ? src[i + 1] << 8 : 0));
    }
    wait_isr(ISR_RDC);
    NE_REG(ISR) = ISR_RDC;
}

/* Copy `len` bytes from the receive ring, wrapping at its end. */
static void ring_read(uint16_t addr, uint8_t *dst, uint16_t len)
{
    uint16_t end = RX_STOP << 8;

    if (addr + len > end) {
        uint16_t first = (uint16_t)(end - addr);
        dma_read(addr, dst, first);
        dst += first;
        len = (uint16_t)(len - first);
        addr = RX_START << 8;
    }
    dma_read(addr, dst, len);
}

static uint8_t read_curr(void)
{
    NE_REG(CR) = CR_NODMA | run | CR_PAGE1;
    uint8_t curr = NE_REG(CURR);
    page0();
    return curr;
}

/* BNRY trails the next frame by one page, so the ring is empty when the
 * page after BNRY is CURR. */
static void receive(void)
{
    for (int budget = 0; budget < 32; budget++) {
        uint8_t next = (uint8_t)(NE_REG(BNRY) + 1);
        if (next >= RX_STOP)
            next = RX_START;
        if (next == read_curr())
            return;

        uint8_t hdr[4];
        dma_read((uint16_t)(next << 8), hdr, sizeof(hdr));
        /* The count covers the header and the frame (as the Linux 8390
         * driver reads it); the frame itself may carry its CRC. */
        uint16_t count = (uint16_t)(hdr[2] | hdr[3] << 8);
        uint8_t after = hdr[1];
        if (after < RX_START || after >= RX_STOP || count < 4 + 14) {
            /* A corrupt header: give up on the ring's contents. */
            net_rx_drop();
            after = read_curr();
        } else {
            uint16_t len = (uint16_t)(count - 4);
            if (len > sizeof(rx_buf))
                len = sizeof(rx_buf);
            ring_read((uint16_t)((next << 8) + 4), rx_buf, len);
            net_receive(rx_buf, len);
        }
        uint8_t bnry = (uint8_t)(after - 1);
        if (bnry < RX_START)
            bnry = RX_STOP - 1;
        NE_REG(BNRY) = bnry;
    }
}

/* Receive-ring overflow recovery (DP8390D datasheet, "Buffer Ring
 * Overflow"): stop, clear the byte count, enter loopback, restart, drain
 * the ring, then leave loopback. */
static void overflow(void)
{
    net_overrun();
    NE_REG(CR) = CR_NODMA | CR_STP;
    wait_isr(ISR_RST);
    NE_REG(RBCR0) = 0;
    NE_REG(RBCR1) = 0;
    NE_REG(TCR) = TCR_LB0;
    page0();
    receive();
    NE_REG(ISR) = ISR_OVW;
    NE_REG(TCR) = 0;
}

void ne2000_irq(void)
{
    if (!present)
        return;
    for (int i = 0; i < 4; i++) {
        uint8_t isr = NE_REG(ISR) & (uint8_t)~ISR_RDC;
        if (!isr)
            break;
        if (isr & ISR_OVW)
            overflow();
        if (isr & (ISR_PRX | ISR_RXE)) {
            NE_REG(ISR) = ISR_PRX | ISR_RXE;
            receive();
        }
        NE_REG(ISR) = isr & (uint8_t)~(ISR_PRX | ISR_RXE | ISR_OVW);
    }
}

static bool ram_works(void)
{
    uint8_t pattern[32], back[32];

    for (unsigned i = 0; i < sizeof(pattern); i++)
        pattern[i] = (uint8_t)(0x5A ^ (i * 37));
    dma_write(TX_PAGE << 8, pattern, sizeof(pattern));
    dma_read(TX_PAGE << 8, back, sizeof(back));
    if (!memcmp(pattern, back, sizeof(back)))
        return true;
    kprintf("ne2000: %s-wide RAM test reads %02x %02x %02x %02x (wrote %02x %02x %02x %02x), ISR %x\n",
            wide ? "word" : "byte", back[0], back[1], back[2], back[3], pattern[0], pattern[1],
            pattern[2], pattern[3], NE_REG(ISR));
    return false;
}

/* The station address PROM, the first 32 bytes of card memory: each byte
 * appears twice from a 16-bit card and once from an 8-bit one, bytes 14 and
 * 15 of the PROM holding 0x57 on an NE2000. */
static bool prom_mac(void)
{
    uint8_t prom[32];
    int step;

    dma_read(0, prom, sizeof(prom));
    if (prom[28] == 0x57 && prom[30] == 0x57)
        step = 2;
    else if (prom[14] == 0x57 && prom[15] == 0x57)
        step = 1;
    else
        return false;
    for (int i = 0; i < 6; i++)
        mac[i] = prom[i * step];
    return true;
}

/* Some CF cards keep that PROM image in attribute memory instead, one byte
 * per even address (Linux pcnet_cs's hw_info table; the AmbiCom AMB8610
 * at 0x1C0, where the CIS showed 00 10 7a .. 57 57). Only below 0x200 is
 * reachable (hw.h). */
static bool attribute_mac(void)
{
    for (unsigned i = 0; i + 16 <= 0x100; i += 16) {
        if (CF_CIS(i + 14) != 0x57 || CF_CIS(i + 15) != 0x57)
            continue;
        uint8_t or = 0, and = 0xFF;
        for (int b = 0; b < 6; b++) {
            mac[b] = CF_CIS(i + b);
            or |= mac[b];
            and &= mac[b];
        }
        if (or && and != 0xFF && !(mac[0] & 1)) {
            kprintf("ne2000: station address from attribute memory %x\n", 2 * i);
            return true;
        }
    }
    return false;
}

void ne2000_init(void)
{
    PCMCIA_BRIDGE = PCMCIA_BRIDGE | PCMCIA_BRIDGE_EN;
    if (!PCMCIA_CARD_PRESENT()) {
        kputs("ne2000: no card\n");
        return;
    }
    /* A DP8390 is always either stopped or started; a floating bus reads
     * 0xFF and a storage card's window reads neither bit. */
    uint8_t cr = NE_REG(CR);
    if (cr == 0xFF || !(cr & (CR_STP | CR_STA))) {
        kputs("ne2000: card is not an NE2000\n");
        return;
    }

    /* Reset: a read of the reset port starts it, a write ends it. */
    uint8_t r = NE_RESET;
    NE_RESET = r;
    wait_isr(ISR_RST);
    NE_REG(ISR) = 0xFF;

    wide = true;
    NE_REG(CR) = CR_NODMA | CR_STP;
    NE_REG(DCR) = DCR_FT1 | DCR_LS | DCR_WTS;
    NE_REG(RBCR0) = 0;
    NE_REG(RBCR1) = 0;
    NE_REG(IMR) = 0;
    NE_REG(ISR) = 0xFF;
    NE_REG(RCR) = 0x20;            /* monitor: receive nothing yet */
    NE_REG(TCR) = TCR_LB0;

    /* The remote DMA width the card answers: a pattern written to its
     * transmit buffer must read back. */
    if (!ram_works()) {
        wide = false;
        NE_REG(DCR) = DCR_FT1 | DCR_LS;
        if (!ram_works()) {
            kputs("ne2000: card is not an NE2000 (its buffer RAM doesn't read back)\n");
            return;
        }
    }
    if (!prom_mac() && !attribute_mac()) {
        kputs("ne2000: no station address in the PROM or attribute memory\n");
        return;
    }

    NE_REG(CR) = CR_NODMA | CR_STP;
    NE_REG(PSTART) = RX_START;
    NE_REG(PSTOP) = RX_STOP;
    NE_REG(BNRY) = RX_START;
    NE_REG(TPSR) = TX_PAGE;
    NE_REG(CR) = CR_NODMA | CR_STP | CR_PAGE1;
    for (int i = 0; i < 6; i++)
        NE_REG(PAR0 + i) = mac[i];
    for (int i = 0; i < 8; i++)
        NE_REG(MAR0 + i) = 0;
    NE_REG(CURR) = RX_START + 1;
    NE_REG(CR) = CR_NODMA | CR_STP;
    NE_REG(ISR) = 0xFF;
    NE_REG(IMR) = ISR_PRX | ISR_RXE | ISR_OVW;
    run = CR_STA;
    NE_REG(CR) = CR_NODMA | run;
    NE_REG(TCR) = 0;
    NE_REG(RCR) = RCR_AB;
    present = true;
    net_set_mac(mac);
    KJ_SRC_PCMCIA |= KJ_SRC_ENABLE;    /* its interrupt; irq_init masked it */
    kprintf("ne2000: MAC %02x:%02x:%02x:%02x:%02x:%02x (%s DMA)\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], wide ? "word" : "byte");
}

bool ne2000_present(void)
{
    return present;
}

/* The card was pulled: mask its interrupt and stop using it (net.c drops
 * its queued frames). */
void ne2000_removed(void)
{
    KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE;
    present = false;
    run = CR_STP;
}

int ne2000_send(const uint8_t *tx_buf, uint32_t len)
{
    dma_write(TX_PAGE << 8, tx_buf, (uint16_t)len);
    NE_REG(TPSR) = TX_PAGE;
    NE_REG(TBCR0) = (uint8_t)len;
    NE_REG(TBCR1) = (uint8_t)(len >> 8);
    NE_REG(CR) = CR_NODMA | CR_TXP | run;
    /* Transmission takes ~1.2 ms at 10 Mbit/s; wait so the next frame
     * doesn't overwrite this one. */
    bool done = wait_isr(ISR_PTX | ISR_TXE);
    uint8_t isr = NE_REG(ISR) & (ISR_PTX | ISR_TXE);
    NE_REG(ISR) = isr;
    if (!done || (isr & ISR_TXE))
        return GZ_EIO;
    return (int)len;
}
