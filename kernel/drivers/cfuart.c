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
#define WINDOW 0x0a00c000u
#define U(r) REG8(io + (r))
#define RX_SIZE 4096u
#define TX_SIZE 4096u
#define IER_RX 0x05u    /* RX available + line status */
#define IER_TX 0x02u
static uint8_t rx[RX_SIZE], tx[TX_SIZE];
static uint32_t rh, rt, th, tt, io;
static uint8_t mcr;
static int fault = GZ_ENOENT;
static void fail(void) { fault = GZ_EIO; U(1) = 0; KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE; }
int cfuart_error(void) { return fault; }
int cfuart_init(const struct uart_config *c)
{
    KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE;
    if (!c->cor_address || c->cor_address >= 0x800u || (c->io_base & 7u) ||
        c->io_base < 0x100u || c->io_base > 0x3f8u || !c->divisor)
        return fault = GZ_EINVAL;
    io = WINDOW + c->io_base;
    /* Socket power/bus policy belongs to cf.c; preserve the live setup. */
    PCMCIA_A020 = (PCMCIA_A020 & ~4u) | PCMCIA_A020_CONFIG;
    REG8(0x0a00d000u + c->cor_address) = c->cor;
    U(3) = (uint8_t)(c->lcr | 0x80u);           /* DLAB */
    U(0) = (uint8_t)c->divisor; U(1) = (uint8_t)(c->divisor >> 8);
    U(3) = c->lcr;
    U(1) = 0; U(2) = c->fcr; U(4) = mcr = c->mcr;
    rh = rt = th = tt = 0; fault = 0;
    if (U(5) == 0xff) return fault = GZ_EIO;
    while (U(5) & 1) (void)U(0);                /* stale bytes from before the reset */
    U(1) = IER_RX;
    KJ_SRC_PCMCIA |= KJ_SRC_ENABLE;
    return 0;
}
void cfuart_poll(void)
{
    if (fault || !PCMCIA_CARD_PRESENT()) return;
    for (unsigned n = 0; n < 256; n++) {
        uint8_t lsr = U(5);
        if (lsr & 0x1e) { fail(); return; } /* overrun/parity/frame/break */
        if (!(lsr & 1)) break;
        uint8_t byte = U(0);
        if (rh - rt == RX_SIZE) { fail(); return; }
        rx[rh++ % RX_SIZE] = byte;
    }
    /* One THR byte per observed empty state; no FIFO capacity assumptions. */
    for (unsigned n = 0; n < 16 && tt != th && (U(5) & 0x20); n++)
        U(0) = tx[tt++ % TX_SIZE];
    U(1) = tt == th ? IER_RX : IER_RX | IER_TX;
    (void)U(2); /* retire IIR after the conditions have been serviced */
    (void)U(6); /* and the modem-status condition */
}
void cfuart_stop(void)
{
    KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE;
    if (io && PCMCIA_CARD_PRESENT()) { U(1) = 0; U(4) = 0; }
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
    U(4) = mcr;
}
uint8_t cfuart_status(void)
{
    return fault || !PCMCIA_CARD_PRESENT() ? 0 : U(6);
}
