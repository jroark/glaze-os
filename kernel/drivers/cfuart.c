/* 16550 UART on a CF serial card (FUNCID 2) in the PCMCIA I/O window.
 * Register meanings follow the NS16550 data sheet; the configuration (COR,
 * port, divisor, line format) comes from the socket dispatcher: the verified
 * Bluetooth card in docs/hardware/BLUETOOTH.md, modems in docs/hardware/MODEM.md.
 * All callbacks execute with interrupts disabled. Protocols run in userspace.
 */
#include "../kernel.h"
#include <glaze/abi.h>
#include "../hw.h"
#include "../module.h"
/* Tests (tests/native/bluetooth_uart.c) substitute the register access. */
#ifndef CF_UART_READ
#define CF_UART_READ(r) REG8(io + (r))
#define CF_UART_WRITE(r,v) (REG8(io + (r)) = (v))
#endif
#define R(r) CF_UART_READ(r)
#define W(r,v) CF_UART_WRITE(r,v)
#define RX_SIZE 4096u
#define TX_SIZE 4096u
#define IER_RX 0x05u    /* RX available + line status */
#define IER_TX 0x02u
static uint8_t rx[RX_SIZE], tx[TX_SIZE];
static uint32_t rh, rt, th, tt, io;
static uint8_t mcr;
static unsigned tx_fifo = 1;
static bool io_logged;
static int fault = GZ_ENOENT;
static void fail(void) { fault = GZ_EIO; W(1, 0); KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE; }
int cfuart_error(void) { return fault; }
int cfuart_init(const struct uart_config *c)
{
    KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE;
    /* The windows of the socket holding the card (hw.h, cf.c): the CF
     * socket's, below its NAND port, or the PCMCIA jacket's (socket 1). */
    bool cf_socket = pcmcia_io_base == PCMCIA0_IO;
    if (!c->cor_address || c->cor_address >= (cf_socket ? 0x800u : 0x200000u) ||
        (c->io_base & 7u) || c->io_base < 0x100u || c->io_base > 0x3f8u || !c->divisor)
        return fault = GZ_EINVAL;
    io = pcmcia_io_base + c->io_base;
    /* Socket power/bus policy belongs to cf.c; preserve the live setup.
     * Socket 1's configuration window is mapped by cf.c (socket1_power_up). */
    if (cf_socket)
        PCMCIA_A020 = (PCMCIA_A020 & ~4u) | PCMCIA_A020_CONFIG;
    REG8(pcmcia_attr_base + c->cor_address) = c->cor;
    W(3, (uint8_t)(c->lcr | 0x80u));            /* DLAB */
    W(0, (uint8_t)c->divisor); W(1, (uint8_t)(c->divisor >> 8));
    W(3, c->lcr);
    W(1, 0); W(2, c->fcr); W(4, mcr = c->mcr);
    /* NS16550 IIR bits 6/7 report functioning FIFOs. A 16550A-compatible
     * FIFO holds at least 16 bytes; retain a one-byte fallback otherwise. */
    unsigned fifo = (R(2) & 0xc0) == 0xc0 ? 16 : 1;
    if (fifo != tx_fifo || !io_logged)   /* once: the GPS search re-arms often */
        kprintf("cfuart: transmit FIFO %u bytes\n", fifo);
    tx_fifo = fifo; io_logged = true;
    rh = rt = th = tt = 0; fault = 0;
    if (R(5) == 0xff) return fault = GZ_EIO;
    for (unsigned n = 0; n < 64 && (R(5) & 1); n++)
        (void)R(0);                             /* stale bytes from before the reset */
    W(1, IER_RX);
    KJ_SRC_PCMCIA |= KJ_SRC_ENABLE;
    return 0;
}
void cfuart_poll(void)
{
    if (fault || !PCMCIA_CARD_PRESENT()) return;
    /* TI TL16C550C SLLS177I, "FIFO interrupt mode operation": reading IIR
     * acknowledges THRE. Do this before filling TX; reading it after a short
     * fill can clear a newly raised interrupt and strand queued bytes. */
    (void)R(2);
    for (unsigned n = 0; n < 256; n++) {
        uint8_t lsr = R(5);
        if (lsr & 0x1e) { fail(); return; } /* overrun/parity/frame/break */
        if (!(lsr & 1)) break;
        uint8_t byte = R(0);
        if (rh - rt == RX_SIZE) { fail(); return; }
        rx[rh++ % RX_SIZE] = byte;
    }
    /* THRE means the entire enabled FIFO is empty, rather than one free
     * slot. Fill its verified minimum capacity in one bounded service pass. */
    if (tt != th && (R(5) & 0x20)) {
        unsigned count = MIN(tx_fifo, th - tt);
        for (unsigned n = 0; n < count; n++) W(0, tx[tt++ % TX_SIZE]);
    }
    W(1, tt == th ? IER_RX : IER_RX | IER_TX);
}
void cfuart_stop(void)
{
    KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE;
    if (io && PCMCIA_CARD_PRESENT()) { W(1, 0); W(4, 0); }
    memset(rx, 0, sizeof(rx)); memset(tx, 0, sizeof(tx));
    rh = rt = th = tt = 0; fault = GZ_ENOENT;
}
int cfuart_read(uint8_t *out, uint32_t count)
{
    if (fault) return fault;
    uint32_t n = MIN(count, rh - rt);
    for (uint32_t i = 0; i < n; i++) out[i] = rx[(rt+i) % RX_SIZE];
    return (int)n;
}
void cfuart_consume(uint32_t count) { rt += MIN(count, rh - rt); }
int cfuart_write(const uint8_t *in, uint32_t count)
{
    if (fault) return fault;
    if (count > TX_SIZE - (th - tt)) return GZ_EBUSY;
    for (uint32_t i = 0; i < count; i++) tx[th++ % TX_SIZE] = in[i];
    cfuart_poll(); return (int)count;
}
void cfuart_set_lines(uint8_t lines)
{
    if (fault || !PCMCIA_CARD_PRESENT()) return;
    mcr = (uint8_t)((mcr & ~3u) | (lines & 3u));
    W(4, mcr);
}
uint8_t cfuart_status(void)
{
    return fault || !PCMCIA_CARD_PRESENT() ? 0 : R(6);
}
