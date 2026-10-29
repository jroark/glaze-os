/* Casio piezo PWM, recovered from the user's stock bltinbuz.dll.
 * 018F119C: period=1411200/Hz-1. 018F11F8: duty=period*(100-pulse)/100-1.
 * 018F1604..1678: shared CMM gate, stop +20, OR config 0202, period +24,
 * duty +28, start +20. 018F2190: halfword zero stops the channel.
 * See docs/BUZZER.md. Never program the backlight's +0/+8/+C channel. */
#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"

static uint64_t buzzer_until;
bool buzzer_active(void) { return buzzer_until && time_us() < buzzer_until; }

void buzzer_init(void)
{
    BUZZ_CONTROL = 0;
    BUZZ_PERIOD = BUZZ_DUTY = 0;
    buzzer_until = 0;
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
