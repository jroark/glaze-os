/*
 * Boot splash progress (boot/splash.h) from kmain until SpringBoard maps the
 * framebuffer. NAND boots only: Glaze's SPL lit the panel, drew the splash
 * and filled the first half of the bar. Stage marks come from kmain; past the
 * last one the bar creeps on from the timer tick while the file server
 * mounts and SpringBoard starts, without ever reaching the end.
 */
#include "kernel.h"
#include "hw.h"
#include "drivers.h"
#include "../boot/splash.h"

#define SPLASH_FB       ((volatile uint16_t *)(0xA0000000u | FB_PA))
#define SPLASH_CREEP_TO 950u

static bool live;
static unsigned shown, ticks;

void splash_start(void)
{
    /* The gate words are always clocked; the LCD controller only with
     * 1100/110C on (a read with them off would hang the bus). */
    live = (DISP_LCD_CLOCK & 1u) && (DISP_LCD_GATE & 1u) &&
        DISP_LCD(0x48) == 0x030Fu;
    shown = 500;
}

bool splash_live(void)
{
    return live;
}

void splash_step(unsigned permille)
{
    if (!live || permille <= shown)
        return;
    shown = permille;
    splash_progress(SPLASH_FB, shown);
}

void splash_tick(void)
{
    if (!live || ++ticks % 25u)
        return;
    unsigned step = (SPLASH_CREEP_TO - shown) / 16u;
    splash_step(shown + (step ? step : shown < SPLASH_CREEP_TO));
}

void splash_end(void)
{
    live = false;
}
