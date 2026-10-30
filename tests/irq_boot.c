/* Production cascade initialization from deliberately wrong inherited state. */
#include "../kernel/kernel.h"
#include "../kernel/drivers.h"
#include <assert.h>
#include <stdio.h>
#define GLAZE_HW_H
static uint16_t sys_status, msys, msys2, mglo, mghi, istatlo, istathi, ienlo, ienhi;
static uint16_t iosel, typ, polarity, hold;
static uint32_t source[17], girq_out;
#define ICU_SYSINT1 sys_status
#define ICU_MSYSINT1 msys
#define ICU_MSYSINT2 msys2
#define ICU_MGIUINTL mglo
#define ICU_MGIUINTH mghi
#define ICU_GIUINTL ienlo
#define SYSINT1_GIU 256
#define GIU_INTENL ienlo
#define GIU_INTENH ienhi
#define GIU_INTSTATL istatlo
#define GIU_INTSTATH istathi
#define GIU_IOSELL iosel
#define GIU_INTTYPL typ
#define GIU_INTALSELL polarity
#define GIU_INTHTSELL hold
#define KJ_GIRQ0_EN(n) source[n]
#define KJ_SRC_PCMCIA source[0]
#define KJ_SRC_KEYS source[1]
#define KJ_SRC_EN source[8]
#define KJ_SRC_ENABLE 8u
#define KJ_SRC_MODE 4u
#define KJ_GIRQ0_OUT girq_out
#define KJ_INTSTAT 0u
#define KJ_INT_CF 1u
#define KJ_INT_KEYS 2u
#define KJ_INT_AUDIO 256u
#define KJ_INT_TOUCH 512u
#define KJ_CF_CAUSE source[16]
#define AUD_SRC_EN_BIT 8u
#define PCMCIA_CARD_PRESENT() 0
#include "../kernel/drivers/irq.c"
void input_irq(uint16_t bits) { (void)bits; }
void net_irq(void) {}
bool net_up(void) { return false; }
void audio_irq(void) {}
void kprintf(const char *s, ...) { (void)s; }
int main(void)
{
    iosel = typ = hold = 0xA5A5; polarity = 0x5A5A;
    ienlo = ienhi = msys = msys2 = mglo = mghi = 0xFFFF;
    for (unsigned i = 0; i < 17; i++) source[i] = 0xABC9;  /* mode bit clear, as after a power-on */
    irq_init();
    assert(iosel == 0xA5A4 && typ == 0xA5A4 && hold == 0xA5A4);
    assert(polarity == 0x5A5B && ienlo == 1 && !ienhi);
    assert(istatlo == 0xFFFF && istathi == 0xFFFF);
    assert(msys == 256 && !msys2 && mglo == 1 && !mghi);
    /* Handled sources masked with CE's mode bit; audio's enable left to audio.c. */
    for (unsigned i = 0; i < 17; i++)
        assert(source[i] == ((i == 0 || i == 1 || i == 9) ? 0xABC5u : i == 8 ? 0xABCDu : 0xABC9u));
    assert(girq_out == 1);
    puts("irq-boot: GPIO0 high-level cascade, unrelated pins preserved, touch masked until ready, GIRQ0 output and source mode on PASS");
}
