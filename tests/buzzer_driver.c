#include "../kernel/kernel.h"
#include "../kernel/drivers.h"
#include "../kernel/hw.h"
#include <assert.h>
#include <stdio.h>
static uint32_t regs[0x1200/4];
static uint64_t now;
static volatile uint32_t *mmio(uint32_t pa)
{
    unsigned off=pa-0x0A000000u;
    assert(off<sizeof(regs) && !(off&3));return &regs[off/4];
}
#undef REG32
#define REG32(pa) (*mmio(pa))
uint64_t time_us(void) { return now; }
#include "../kernel/drivers/buzzer.c"
int main(void)
{
    struct proc audio={.flags=PROC_AUDIO},other={0};
    DISP_BL_CONTROL=1;DISP_BL_CONFIG=0x80000002;DISP_BL_PERIOD=0xf70;
    DISP_BACKLIGHT=981;DISP_BL_GATE=0xa5000001;regs[0x880/4]=0x1234;
    BUZZ_CONTROL=1;buzzer_init();assert(!BUZZ_CONTROL && !BUZZ_PERIOD && !BUZZ_DUTY);
    assert(sys_buzzer_tone(&other,1000,100,50)==GZ_EPERM);
    assert(sys_buzzer_tone(&audio,1000,100,50)==0);
    assert(BUZZ_CONTROL==1 && BUZZ_PERIOD==1410 && BUZZ_DUTY==704);
    assert(buzzer_active());
    assert(DISP_BL_CONTROL==1 && DISP_BL_PERIOD==0xf70 && DISP_BACKLIGHT==981);
    assert(DISP_BL_CONFIG==0x80000202 && DISP_BL_GATE==0xa5000001 && regs[0x880/4]==0x1234);
    assert(sys_buzzer_tone(&audio,0xffffffff,100,50)==GZ_EINVAL && BUZZ_CONTROL==1);
    assert(sys_buzzer_tone(&audio,1000,1001,50)==GZ_EINVAL && BUZZ_CONTROL==1);
    assert(sys_buzzer_tone(&audio,1000,100,51)==GZ_EINVAL && BUZZ_CONTROL==1);
    now=99999;buzzer_poll();assert(BUZZ_CONTROL==1);
    now=100000;assert(!buzzer_active());buzzer_poll();assert(!BUZZ_CONTROL);
    DISP_BL_CONTROL=0;DISP_BL_GATE&=~1u;DISP_BL_CONFIG=0;
    assert(!sys_buzzer_tone(&audio,2000,20,35));
    assert(!DISP_BL_CONTROL && (DISP_BL_GATE&1) && DISP_BL_CONFIG==0x202);
    assert(!sys_buzzer_tone(&audio,0,0,0) && !BUZZ_CONTROL);
    assert(sys_buzzer_tone(&other,0,0,0)==GZ_EPERM);
    assert(sys_buzzer_tone(&audio,1000,0,50)==GZ_EINVAL);
    assert(sys_buzzer_tone(&audio,1000,100,0)==GZ_EINVAL);
    puts("buzzer: recovered PWM, permissions, bounds, deadlines and independent display/audio PASS");
}
