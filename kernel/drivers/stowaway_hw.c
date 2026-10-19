/* BE-300 companion SIU: hardware.txt line 191, be300 documentation
 * EMULATED_HARDWARE_LINUX_PORTING.md, UARTs (stride 4 at PA 0x0A008680).
 * This board-specific UART is not described by the generic VRC4173 UM.
 * Stowaway.dll 0x1036c0 configures 9600 8N1; 0x103dd4 checks FA FD.
 * DTR + RTS rising-edge probe follows be300/src/stowaway.c.
 * RX is polled on the existing 100 Hz tick; no WinCE latch tricks required.
 */
#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"
#include "stowaway.h"
#define U(n) REG8(UART_BASE + (n) * 4u)
static struct stowaway_decoder decoder;
static bool connected;

int stowaway_status(void) { return connected ? GZ_INPUT_STOWAWAY : 0; }
static void emit(uint16_t type, uint16_t code) { input_push(type, code, 0, 0); }

void stowaway_init(void)
{
    uint8_t lcr = U(3), mcr = U(4);
    U(3) = lcr & ~0x80u;
    uint8_t ier = U(1);
    U(1) = 0;                       /* polling, including during the probe */
    U(3) = 0x83;                    /* DLAB, 8N1 */
    uint8_t dll = U(0), dlm = U(1);
    U(0) = 120; U(1) = 0;           /* linux4be serial_be300.c baud_table:
                                     * SIU baud clock 1.152 MHz / 120 = 9600 */
    U(3) = 3;
    for (int n = 0; n < 128 && (U(5) & 1); n++)
        (void)U(0);
    U(4) = 1;                       /* DTR, RTS low */
    uint32_t start = (uint32_t)(time_us() / 1000u);
    /* Allow dock power to settle before the RTS edge. */
    while ((uint32_t)(time_us() / 1000u) - start < 20)
        ;
    U(4) = 3;
    unsigned ack = 0;
    while ((uint32_t)(time_us() / 1000u) - start < 1000) {
        if ((U(5) & 1) && stowaway_ack(&ack, U(0))) {
            /* 16550 FCR bit 0 (be300/gxemul thirdparty/comreg.h):
             * buffer bursts between 10 ms polls; leave queued bytes intact. */
            U(2) = 1;
            connected = true;
            console_use_debug_uart();
            kputs("input: Stowaway keyboard detected\n");
            return;
        }
    }
    U(4) = mcr;
    U(3) = 0x83;
    U(0) = dll; U(1) = dlm;
    U(3) = lcr & ~0x80u;
    U(1) = ier;
    U(3) = lcr;
    kputs("input: no Stowaway keyboard\n");
}

void stowaway_poll(void)
{
    if (!connected)
        return;
    uint32_t now = (uint32_t)(time_us() / 1000u);
    for (int n = 0; n < 128; n++) {
        uint8_t lsr = U(5);
        if (lsr & 0x1e)             /* parity/framing/break/overrun: reset modifiers */
            memset(&decoder, 0, sizeof(decoder));
        if (!(lsr & 1))
            break;
        uint8_t byte = U(0);
        if (!(lsr & 0x1e))
            stowaway_decode(&decoder, byte, now, emit);
    }
    stowaway_repeat(&decoder, now, emit);
}
