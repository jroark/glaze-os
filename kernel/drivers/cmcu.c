/*
 * Casio's power-management microcontroller, through the companion's ScCmcu
 * window at 0x0A007800 (hardware.txt:189). Nothing documents its protocol;
 * this replays what Windows CE's driver does, traced in the emulator (user
 * code at 0x019F19C4-0x019F2028):
 *
 *   once:           CONFIG (0x7810) = 0x10000 (bit 16 reads set after the
 *                   watchdog reset the system, and CE logs "System was
 *                   rebooted by WDT."; writing 1 clears it:
 *                   cedmbltin.dll 0x019F1FE8-0x019F2028)
 *   every ~9 s:     TIMEOUT (0x7814) = 0x96
 *   about 10 / s:   ARG (0x780C) = 0x53, then CMD (0x7808) = 0x3A
 *
 * each after reading STATUS (0x7834). A real BE-300 reads 3 there after
 * Windows CE (as be300 does while the watchdog runs), whose driver evidently
 * doesn't wait for 0, so the read is only replayed. TIMEOUT is a watchdog reload in
 * seconds (0x96 = 150): a real BE-300 resets 140-150 s after Windows CE
 * stops unless something keeps writing it, and with this it keeps running.
 *
 * Before it sleeps, Windows CE stops the watchdog, and it starts it again
 * on waking (cedmbltin.dll CDM_PowerDown 0x019F1430 and CDM_PowerUp
 * 0x019F1454, with StopWDT 0x019F1A6C and the start loop 0x019F1990):
 *
 *   stop:   ARG0 (0x7804) = 0x35, CMD0 (0x7800) = 0x5C until STATUS & 7 == 0
 *   start:  TIMEOUT = seconds, then ARG = 0x53, CMD = 0x3A until STATUS & 7 == 3
 *
 * CE tries a stop up to 1000 times. be300 models the same STATUS values
 * (3 running, 0 stopped; be300 src/hw/cmcu.c).
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

#define CMCU_TRIES      1000u

bool cmcu_stop(void)
{
    for (uint32_t n = 0; n < CMCU_TRIES; n++) {
        CMCU_ARG0 = 0x35u;
        CMCU_CMD0 = 0x5Cu;
        if ((CMCU_STATUS & 7u) == 0)
            return true;
    }
    return false;
}

void cmcu_start(void)
{
    CMCU_TIMEOUT = RELOAD_SECONDS;
    for (uint32_t n = 0; n < CMCU_TRIES; n++) {
        CMCU_ARG = 0x53u;
        CMCU_CMD = 0x3Au;
        if ((CMCU_STATUS & 7u) == 3)
            break;
    }
    ticks = 0;
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
