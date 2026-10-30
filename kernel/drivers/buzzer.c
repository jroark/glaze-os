/* Casio piezo PWM, recovered from the user's stock bltinbuz.dll.
 * 018F119C: period=1411200/Hz-1. 018F11F8: duty=period*(100-pulse)/100-1.
 * 018F1604..1678: shared CMM gate, stop +20, OR config 0202, period +24,
 * duty +28, start +20. 018F2190: halfword zero stops the channel.
 * See docs/BUZZER.md. Never program the backlight's +0/+8/+C channel. */
#include "../kernel.h"
#include "../hw.h"
#include "../mips.h"
#include "../drivers.h"

static uint64_t buzzer_until;
bool buzzer_active(void) { return buzzer_until && time_us() < buzzer_until; }

void buzzer_init(void)
{
    BUZZ_CONTROL = 0;
    BUZZ_PERIOD = BUZZ_DUTY = 0;
    buzzer_until = 0;
}
/*
 * Audible boot checkpoints (BOOT_BEEP=1 builds): n short 2 kHz beeps,
 * timed by CP0 Count since they come before timer_init. A power-on boot
 * has its LCD off until display_init, so this is the only progress signal
 * then. The shared BLG gate and configuration are put back afterwards.
 */
static void count_wait(uint32_t ms)
{
    uint32_t t0 = read_c0_count();
    while (read_c0_count() - t0 < ms * 10000u)     /* Count runs near 10 MHz */
        ;
}

void buzzer_beeps(unsigned n)
{
    uint32_t gate = DISP_BL_GATE, config = DISP_BL_CONFIG;
    uint32_t period = GZ_BUZZER_CLOCK / 2000 - 1;

    DISP_BL_GATE = gate | 1;
    DISP_BL_CONFIG = config | 0x0202;
    for (unsigned i = 0; i < n; i++) {
        BUZZ_PERIOD = period;
        BUZZ_DUTY = period / 2;
        BUZZ_CONTROL = 1;
        count_wait(120);
        BUZZ_CONTROL = 0;
        count_wait(180);
    }
    count_wait(700);
    DISP_BL_CONFIG = config;
    DISP_BL_GATE = gate;
}

void buzzer_poll(void)
{
    if (buzzer_until && time_us() >= buzzer_until) {
        BUZZ_CONTROL = 0;
        buzzer_until = 0;
    }
}
int sys_buzzer_tone(struct proc *p, uint32_t hz, uint32_t ms, uint32_t pulse)
{
    if (!(p->flags & PROC_AUDIO)) return GZ_EPERM;
    if (hz && (hz < 80 || hz > 6000 || !ms || ms > GZ_BUZZER_MAX_MS || !pulse || pulse > 50))
        return GZ_EINVAL;
    BUZZ_CONTROL = 0;
    buzzer_until = 0;
    if (!hz) return 0;
    uint32_t period = GZ_BUZZER_CLOCK / hz - 1;
    DISP_BL_GATE |= 1;
    DISP_BL_CONFIG |= 0x0202;
    BUZZ_PERIOD = period;
    BUZZ_DUTY = period * (100 - pulse) / 100 - 1;
    buzzer_until = time_us() + (uint64_t)ms * 1000;
    BUZZ_CONTROL = 1;
    return 0;
}
