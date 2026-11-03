/* SMART/Brainboxes 0160:aba0, BCSP r9. Verified hardware values and CIS:
 * docs/hardware/BLUETOOTH.md. UART registers follow NS16550 definitions.
 * All callbacks execute with interrupts disabled. Protocol runs in userspace.
 */
#include "../kernel.h"
#include <glaze/abi.h>
#include "../hw.h"
#ifndef BT_UART_READ
#define BT_UART_READ(r) REG8(0x0a00c3f8u + (r))
#define BT_UART_WRITE(r,v) (REG8(0x0a00c3f8u + (r))=(v))
#endif
#define R(r) BT_UART_READ(r)
#define W(r,v) BT_UART_WRITE(r,v)
#define RX_SIZE 4096u
#define TX_SIZE 1024u
static uint8_t rx[RX_SIZE], tx[TX_SIZE];
static uint32_t rh, rt, th, tt;
static int fault;
static unsigned tx_fifo=1;
static void fail(void) { fault = GZ_EIO; W(1,0); KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE; }
int btuart_error(void) { return fault; }
int btuart_init(void)
{
    KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE;
    /* Socket power/bus policy belongs to cf.c; preserve the live setup. */
    PCMCIA_A020 = (PCMCIA_A020 & ~4u) | PCMCIA_A020_CONFIG;
    REG8(0x0a00d0e0u) = 0x43; /* verified CIS entry 3: I/O 3f8h, eight ports */
    W(3,0x9b); W(0,1); W(1,0); W(3,0x1b); /* divisor 1, 8E1 */
    W(1,0); W(2,7); W(4,0x0b);
    /* NS16550 IIR bits 6/7 report functioning FIFOs. A 16550A-compatible
     * FIFO holds at least 16 bytes; retain a one-byte fallback otherwise. */
    tx_fifo=(R(2)&0xc0)==0xc0?16:1;
    kprintf("bluetooth: UART transmit FIFO %u bytes\n",tx_fifo);
    rh = rt = th = tt = 0; fault = 0;
    if (R(5) == 0xff) return fault = GZ_EIO;
    W(1,5); /* RX available + line status; trigger one byte */
    KJ_SRC_PCMCIA |= KJ_SRC_ENABLE;
    return 0;
}
void btuart_poll(void)
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
    if(tt!=th && (R(5)&0x20)){
        unsigned count=MIN(tx_fifo,th-tt);
        for(unsigned n=0;n<count;n++)W(0,tx[tt++%TX_SIZE]);
    }
    W(1,tt == th ? 5 : 7);
}
void btuart_stop(void)
{
    KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE;
    if (PCMCIA_CARD_PRESENT()) { W(1,0); W(4,0); }
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
