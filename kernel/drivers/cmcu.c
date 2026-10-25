/*
 * Casio's power-management microcontroller, through the companion's ScCmcu
 * window at 0x0A007800 (hardware.txt:189). Nothing documents its protocol;
 * this replays what Windows CE's driver does, traced in the emulator (user
 * code at 0x019F19C4-0x019F2028):
 *
 *   once:           CONFIG (0x7810) = 0x10000
 *   every ~9 s:     TIMEOUT (0x7814) = 0x96
 *   about 10 / s:   ARG (0x780C) = 0x53, then CMD (0x7808) = 0x3A
 *
 * each after reading STATUS (0x7834). The emulator returns 0 there, but a
 * real BE-300 reads 3 after Windows CE, whose driver evidently doesn't wait
 * for 0, so the read is only replayed. TIMEOUT is a watchdog reload in
 * seconds (0x96 = 150): a real BE-300 resets 140-150 s after Windows CE
 * stops unless something keeps writing it, and with this it keeps running.
 */

#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"

#define TICK_HZ         100u
#define POLL_TICKS      (TICK_HZ / 10)  /* the 0x53/0x3A pair, as Windows CE */
#define RELOAD_TICKS    (5 * TICK_HZ)   /* TIMEOUT, more often than its 9 s */
#define RELOAD_SECONDS  0x96u

static uint32_t ticks;

void cmcu_init(void)
{
    (void)CMCU_CONFIG;
    CMCU_CONFIG = 0x10000u;
}

void cmcu_tick(void)
{
    ticks++;
    if (ticks % RELOAD_TICKS == 0) {
        (void)CMCU_STATUS;
        CMCU_TIMEOUT = RELOAD_SECONDS;
    }
    if (ticks % POLL_TICKS == 0) {
        (void)CMCU_STATUS;
        CMCU_ARG = 0x53u;
        CMCU_CMD = 0x3Au;
    }
}
