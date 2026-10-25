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
#define DCR_LS      0x08    /* normal operation, not loopback */
#define DCR_FT1     0x40    /* FIFO threshold 8 bytes */
#define RCR_AB      0x04    /* broadcast */
#define TCR_LB0     0x02    /* internal loopback */

/* Card RAM pages (256 bytes each) 0x40..0x7F: six for transmit, the rest
 * for the receive ring. */
#define TX_PAGE     0x40
#define RX_START    0x46
#define RX_STOP     0x80

#define QUEUE_LEN   8
#define FRAME_MIN   60

struct frame {
    uint16_t len;
    uint8_t data[1536];
};

static bool present;
static uint8_t mac[6];
static int bound_port;
static struct frame queue[QUEUE_LEN];
static uint32_t q_head, q_tail;
static uint32_t rx_frames, tx_frames, rx_dropped, overruns;
static uint8_t tx_buf[1536] __attribute__((aligned(4)));
static uint8_t run = CR_STP;        /* CR_STA once the NIC is started */

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

/* Remote DMA moves 16-bit words; odd lengths transfer one extra byte. */
static void dma_read(uint16_t addr, void *dst, uint16_t len)
{
    uint8_t *d = dst;
    uint16_t n = (uint16_t)((len + 1) & ~1);

    page0();
    NE_REG(RBCR0) = (uint8_t)n;
    NE_REG(RBCR1) = (uint8_t)(n >> 8);
    NE_REG(RSAR0) = (uint8_t)addr;
    NE_REG(RSAR1) = (uint8_t)(addr >> 8);
    NE_REG(CR) = CR_RREAD | run;
    for (uint16_t i = 0; i < n; i += 2) {
        uint16_t w = NE_DATA16;
        d[i] = (uint8_t)w;
        if (i + 1 < len)
            d[i + 1] = (uint8_t)(w >> 8);
    }
    wait_isr(ISR_RDC);
    NE_REG(ISR) = ISR_RDC;
}

static void dma_write(uint16_t addr, const uint8_t *src, uint16_t len)
{
    uint16_t n = (uint16_t)((len + 1) & ~1);

    page0();
    NE_REG(ISR) = ISR_RDC;
    NE_REG(RBCR0) = (uint8_t)n;
    NE_REG(RBCR1) = (uint8_t)(n >> 8);
    NE_REG(RSAR0) = (uint8_t)addr;
    NE_REG(RSAR1) = (uint8_t)(addr >> 8);
    NE_REG(CR) = CR_RWRITE | run;
    for (uint16_t i = 0; i < n; i += 2)
        NE_DATA16 = (uint16_t)(src[i] | (i + 1 < len ? src[i + 1] << 8 : 0));
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
            rx_dropped++;
            after = read_curr();
        } else {
            uint16_t len = (uint16_t)(count - 4);
            if (len > sizeof(queue[0].data))
                len = sizeof(queue[0].data);
            if (q_head - q_tail >= QUEUE_LEN) {
                rx_dropped++;
            } else {
                struct frame *f = &queue[q_head % QUEUE_LEN];
                ring_read((uint16_t)((next << 8) + 4), f->data, len);
                f->len = len;
                q_head++;
                rx_frames++;
            }
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
    overruns++;
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
    uint32_t before = q_head;
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
    if (q_head != before && bound_port > 0)
        ipc_notify(bound_port, GZ_NOTIFY_NET);
}

void ne2000_init(void)
{
    PCMCIA_BRIDGE = PCMCIA_BRIDGE | PCMCIA_BRIDGE_EN;
    if ((PCMCIA_CARD_STATUS & PCMCIA_STATUS_MASK) != PCMCIA_CARD_IN) {
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

    NE_REG(CR) = CR_NODMA | CR_STP;
    NE_REG(DCR) = DCR_FT1 | DCR_LS | DCR_WTS;
    NE_REG(RBCR0) = 0;
    NE_REG(RBCR1) = 0;
    NE_REG(IMR) = 0;
    NE_REG(ISR) = 0xFF;
    NE_REG(RCR) = 0x20;            /* monitor: receive nothing yet */
    NE_REG(TCR) = TCR_LB0;

    /* The station address PROM is the first 32 bytes of card memory, each
     * byte doubled; bytes 14 and 15 are 0x57 on an NE2000. */
    uint8_t prom[32];
    dma_read(0, prom, sizeof(prom));
    if (prom[28] != 0x57 || prom[30] != 0x57) {
        kprintf("ne2000: card is not an NE2000 (PROM %x %x)\n", prom[28], prom[30]);
        return;
    }
    for (int i = 0; i < 6; i++)
        mac[i] = prom[i * 2];

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
    KJ_SRC_PCMCIA |= KJ_SRC_ENABLE;    /* its interrupt; irq_init masked it */
    kprintf("ne2000: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool ne2000_present(void)
{
    return present;
}

static int check(struct proc *p)
{
    if (!(p->flags & PROC_NET))
        return GZ_EPERM;
    return present ? 0 : GZ_ENOENT;
}

int ne2000_info(struct proc *p, uint32_t uout)
{
    struct gz_netif_info info;

    if (!(p->flags & PROC_NET))
        return GZ_EPERM;
    memset(&info, 0, sizeof(info));
    info.present = present;
    info.kind = present ? GZ_NET_NE2000 : GZ_NET_NONE;
    memcpy(info.mac, mac, sizeof(mac));
    info.rx_frames = rx_frames;
    info.tx_frames = tx_frames;
    info.rx_dropped = rx_dropped;
    info.overruns = overruns;
    return copy_to_as(&p->as, uout, &info, sizeof(info));
}

int ne2000_bind(struct proc *p, int port)
{
    int rv = check(p);

    if (rv)
        return rv;
    bound_port = port;
    if (q_head != q_tail)
        ipc_notify(port, GZ_NOTIFY_NET);
    return 0;
}

int ne2000_send(struct proc *p, uint32_t ubuf, uint32_t len)
{
    int rv = check(p);

    if (rv)
        return rv;
    if (len < 14 || len > GZ_ETH_FRAME_MAX)
        return GZ_EINVAL;
    if (copy_from_as(&p->as, tx_buf, ubuf, len))
        return GZ_EINVAL;
    if (len < FRAME_MIN) {
        memset(tx_buf + len, 0, FRAME_MIN - len);
        len = FRAME_MIN;
    }
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
    tx_frames++;
    return (int)len;
}

int ne2000_recv(struct proc *p, uint32_t ubuf, uint32_t max)
{
    int rv = check(p);

    if (rv)
        return rv;
    if (q_head == q_tail)
        return 0;
    struct frame *f = &queue[q_tail % QUEUE_LEN];
    uint32_t n = MIN(f->len, max);
    if (copy_to_as(&p->as, ubuf, f->data, n))
        return GZ_EINVAL;
    q_tail++;
    return (int)n;
}
