/*
 * PC Card modems and other serial cards (FUNCID 2): a 16550-compatible UART
 * at the I/O range the card's CIS names, for the network server's PPP
 * dialer (servers/net/modem.c). The network server drives it through the
 * ordinary network syscalls (glaze/modem.h): SYS_NET_SEND/RECV move bytes,
 * SYS_WIFI_CTL raises and drops DTR and reads the modem status lines.
 *
 * Registers follow the NS16550A datasheet: RBR/THR 0, IER 1, IIR/FCR 2,
 * LCR 3, MCR 4, LSR 5, MSR 6, SCR 7, the divisor latch at 0/1 with LCR bit
 * 7, from a 1.8432 MHz clock (115200 baud at divisor 1). The interrupt and
 * the 100 Hz tick both drain the receive FIFO (net_irq); RTS drops while
 * the receive ring is nearly full, so a modem with hardware flow control
 * pauses rather than overrun it.
 *
 * cf.c chooses the card's configuration entry and I/O base from its CIS
 * (pcmcia_modem_config) before this module is loaded.
 */
#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"
#include "../mips.h"
#include <glaze/modem.h>

#define RBR 0
#define THR 0
#define IER 1
#define DLL 0
#define DLM 1
#define FCR 2
#define IIR 2
#define LCR 3
#define MCR 4
#define LSR 5
#define MSR 6
#define SCR 7

#define LSR_DR      0x01
#define LSR_OE      0x02
#define LSR_ERRORS  0x1E
#define LSR_THRE    0x20
#define MCR_DTR     0x01
#define MCR_RTS     0x02
#define MCR_OUT2    0x08        /* gates the interrupt on PC-style UARTs */
#define IER_RX      0x01
#define IER_TX      0x02
#define IER_LINE    0x04
#define IER_MODEM   0x08

#define RX_SIZE     8192u
#define TX_SIZE     2048u
#define RX_HIGH     (RX_SIZE - 1024u)   /* drop RTS above this */
#define RX_LOW      (RX_SIZE / 2)       /* raise it again below this */

static const struct modem_config *config;
static uint32_t base;
static bool present;
static int bound_port;
static struct gz_netif_info nic;
static struct modem_status status;
static uint8_t rx[RX_SIZE], tx[TX_SIZE];
static uint32_t rh, rt, th, tt;
static uint8_t mcr, last_msr;
static bool fifo;               /* a 16550A: 16-byte FIFOs */

static uint8_t in(unsigned r) { return REG8(base + r); }
static void out(unsigned r, uint8_t v) { REG8(base + r) = v; }
static void notify(void) { if (bound_port > 0) ipc_notify(bound_port, GZ_NOTIFY_NET); }

static void set_baud(uint32_t baud)
{
    uint32_t divisor = 115200u / baud;
    uint8_t lcr = in(LCR);
    out(LCR, lcr | 0x80);
    out(DLL, (uint8_t)divisor);
    out(DLM, (uint8_t)(divisor >> 8));
    out(LCR, lcr & 0x7F);
    status.baud = baud;
}

static void set_mcr(void) { out(MCR, mcr); status.dtr = !!(mcr & MCR_DTR); }

/* Move bytes: everything the receive FIFO holds, then as much as the
 * transmitter takes while CTS allows. */
void modem_irq(void)
{
    if (!present)
        return;
    bool changed = false;
    for (unsigned n = 0; n < 512; n++) {
        uint8_t lsr = in(LSR);
        if (lsr == 0xFF) { present = false; return; }   /* the card is gone */
        if (lsr & LSR_ERRORS) {
            if (lsr & LSR_OE) status.rx_overruns++;
        }
        if (!(lsr & LSR_DR))
            break;
        uint8_t byte = in(RBR);
        if (rh - rt == RX_SIZE) {
            status.rx_overruns++;
            continue;
        }
        rx[rh++ % RX_SIZE] = byte;
        status.rx_bytes++;
        changed = true;
    }
    if ((mcr & MCR_RTS) && rh - rt > RX_HIGH) {
        mcr &= ~MCR_RTS;
        set_mcr();
    }
    uint8_t msr = in(MSR);
    status.msr = msr & 0xF0;
    if ((msr ^ last_msr) & (MODEM_DCD | MODEM_DSR))
        changed = true;
    last_msr = msr;
    if (tt != th && (msr & MODEM_CTS) && (in(LSR) & LSR_THRE)) {
        for (unsigned n = fifo ? 16 : 1; n && tt != th; n--) {
            out(THR, tx[tt++ % TX_SIZE]);
            status.tx_bytes++;
        }
    }
    out(IER, IER_RX | IER_LINE | IER_MODEM | (tt != th ? IER_TX : 0));
    (void)in(IIR);
    if (changed)
        notify();
}

bool modem_init(void)
{
    present = false;
    config = pcmcia_modem_config();
    if (!config || !config->port) {
        kputs("modem: no serial configuration from the CIS\n");
        return false;
    }
    base = pcmcia_io_base + config->port;
    KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE;
    out(IER, 0);
    out(SCR, 0x5A);
    if (in(SCR) != 0x5A || in(LSR) == 0xFF) {
        kprintf("modem: no UART at port %x (SCR %x LSR %x)\n", config->port, in(SCR), in(LSR));
        return false;
    }
    out(LCR, 0x03);                     /* 8N1 */
    set_baud(57600);
    out(FCR, 0x87);                     /* FIFO on, both reset, trigger at 8 */
    fifo = (in(IIR) & 0xC0) == 0xC0;
    if (!fifo)
        out(FCR, 0);
    mcr = MCR_DTR | MCR_RTS | MCR_OUT2;
    set_mcr();
    while (in(LSR) & LSR_DR)
        (void)in(RBR);
    rh = rt = th = tt = 0;
    memset(&nic, 0, sizeof(nic));
    nic.present = 1;
    nic.kind = GZ_NET_MODEM;
    status.version = MODEM_STATUS_VERSION;
    status.port = config->port;
    status.rx_bytes = status.tx_bytes = status.rx_overruns = 0;
    memcpy(status.identification, config->identification, sizeof(status.identification));
    status.identification[sizeof(status.identification) - 1] = 0;
    last_msr = in(MSR);
    status.msr = last_msr & 0xF0;
    present = true;
    out(IER, IER_RX | IER_LINE | IER_MODEM);
    KJ_SRC_PCMCIA |= KJ_SRC_ENABLE;
    kprintf("modem: 16550%s at port %x, %u baud, \"%s\"\n", fifo ? "A" : "", config->port,
            status.baud, status.identification);
    return true;
}

bool modem_present(void) { return present; }

void modem_removed(void)
{
    KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE;
    if (present && PCMCIA_CARD_PRESENT()) {
        out(IER, 0);
        out(MCR, 0);                    /* DTR off: the modem hangs up */
    }
    present = false;
    rh = rt = th = tt = 0;
    memset(rx, 0, sizeof(rx));
    memset(tx, 0, sizeof(tx));
    notify();
    bound_port = 0;
}

static int check(struct proc *p)
{
    return !(p->flags & PROC_NET) ? GZ_EPERM : present ? 0 : GZ_ENOENT;
}

int modem_info(struct proc *p, uint32_t out_address)
{
    if (!(p->flags & PROC_NET)) return GZ_EPERM;
    nic.rx_frames = status.rx_bytes;
    nic.tx_frames = status.tx_bytes;
    nic.overruns = status.rx_overruns;
    return copy_to_as(&p->as, out_address, &nic, sizeof(nic));
}

int modem_bind(struct proc *p, int port)
{
    int rv = check(p);
    if (rv) return rv;
    bound_port = port;
    if (rh != rt) notify();
    return 0;
}

/* Bytes to send; fewer than asked when the ring is full (0: try later). */
int modem_send(struct proc *p, uint32_t buf, uint32_t len)
{
    int rv = check(p);
    if (rv) return rv;
    uint8_t chunk[256];
    uint32_t done = 0;
    uint32_t flags = irq_save();
    while (done < len && th - tt < TX_SIZE) {
        uint32_t n = MIN(MIN(len - done, sizeof(chunk)), TX_SIZE - (th - tt));
        if (copy_from_as(&p->as, chunk, buf + done, n)) { rv = GZ_EINVAL; break; }
        for (uint32_t i = 0; i < n; i++)
            tx[th++ % TX_SIZE] = chunk[i];
        done += n;
    }
    modem_irq();
    irq_restore(flags);
    return rv ? rv : (int)done;
}

int modem_recv(struct proc *p, uint32_t buf, uint32_t max)
{
    int rv = check(p);
    if (rv) return rv;
    uint8_t chunk[256];
    uint32_t flags = irq_save();
    modem_irq();
    uint32_t n = MIN(MIN(max, rh - rt), sizeof(chunk));
    for (uint32_t i = 0; i < n; i++)
        chunk[i] = rx[(rt + i) % RX_SIZE];
    irq_restore(flags);
    if (n && copy_to_as(&p->as, buf, chunk, n))
        return GZ_EINVAL;
    flags = irq_save();
    rt += n;
    if (!(mcr & MCR_RTS) && rh - rt < RX_LOW) {
        mcr |= MCR_RTS;
        set_mcr();
    }
    irq_restore(flags);
    return (int)n;
}

int modem_control(struct proc *p, uint32_t op, uint32_t arg)
{
    int rv = check(p);
    if (rv) return rv;
    uint32_t flags = irq_save();
    modem_irq();
    switch (op) {
    case MODEM_STATUS:
        irq_restore(flags);
        return copy_to_as(&p->as, arg, &status, sizeof(status));
    case MODEM_DTR:
        mcr = arg ? (mcr | MCR_DTR) : (mcr & ~MCR_DTR);
        set_mcr();
        if (!arg) th = tt = 0;          /* nothing more goes to a hung-up line */
        break;
    case MODEM_BAUD:
        if (arg < 300 || arg > 115200 || 115200u % arg) rv = GZ_EINVAL;
        else set_baud(arg);
        break;
    default:
        rv = GZ_EINVAL;
    }
    irq_restore(flags);
    return rv;
}
