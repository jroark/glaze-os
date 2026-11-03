/* SMART/Brainboxes 0160:aba0, BCSP r9. Verified hardware values and CIS:
 * docs/hardware/BLUETOOTH.md. UART registers follow NS16550 definitions.
 * All callbacks execute with interrupts disabled. Protocol runs in userspace.
 */
#include "../kernel.h"
#include <glaze/abi.h>
#include "../hw.h"
#define U(r) REG8(0x0a00c3f8u + (r))
#define RX_SIZE 4096u
#define TX_SIZE 1024u
static uint8_t rx[RX_SIZE], tx[TX_SIZE];
static uint32_t rh, rt, th, tt;
static int fault;
static void fail(void) { fault = GZ_EIO; U(1) = 0; KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE; }
int btuart_error(void) { return fault; }
int btuart_init(void)
{
    KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE;
    /* Socket power/bus policy belongs to cf.c; preserve the live setup. */
    PCMCIA_A020 = (PCMCIA_A020 & ~4u) | PCMCIA_A020_CONFIG;
    REG8(0x0a00d0e0u) = 0x43; /* verified CIS entry 3: I/O 3f8h, eight ports */
    U(3) = 0x9b; U(0) = 1; U(1) = 0; U(3) = 0x1b; /* divisor 1, 8E1 */
    U(1) = 0; U(2) = 7; U(4) = 0x0b;
    rh = rt = th = tt = 0; fault = 0;
    if (U(5) == 0xff) return fault = GZ_EIO;
    U(1) = 5; /* RX available + line status; trigger one byte */
    KJ_SRC_PCMCIA |= KJ_SRC_ENABLE;
    return 0;
}
void btuart_poll(void)
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
    U(1) = tt == th ? 5 : 7;
    (void)U(2); /* retire IIR after the conditions have been serviced */
}
void btuart_stop(void)
{
    KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE;
    if (PCMCIA_CARD_PRESENT()) { U(1) = 0; U(4) = 0; }
    memset(rx, 0, sizeof(rx)); memset(tx, 0, sizeof(tx));
    rh = rt = th = tt = 0; fault = GZ_ENOENT;
}
int btuart_read(uint8_t *out, uint32_t count)
{
    if (fault) return fault;
    uint32_t n = MIN(count, rh - rt);
    for (uint32_t i = 0; i < n; i++) out[i] = rx[(rt+i) % RX_SIZE];
    return (int)n;
}
void btuart_consume(uint32_t count) { rt += MIN(count, rh - rt); }
int btuart_write(const uint8_t *in, uint32_t count)
{
    if (fault) return fault;
    if (count > TX_SIZE - (th - tt)) return GZ_EBUSY;
    for (uint32_t i = 0; i < count; i++) tx[th++ % TX_SIZE] = in[i];
    btuart_poll(); return (int)count;
}
