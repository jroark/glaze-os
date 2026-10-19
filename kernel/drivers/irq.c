/*
 * IP2 interrupt cascade:
 *
 *   MIPS IP2 <- VR4131 ICU SYSINT1 bit 8 (GIU) <- GIU line 0
 *            <- VRC4173 GIRQ0 status 0x0A000004: bit 1 buttons, bit 9 touch
 *
 * GIRQ0 bit 0 is the PCMCIA card, the NE2000 when one is inserted.
 *
 * (doc: Interrupt Topology; same flow as the Linux port's irq.c.)
 */

#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"

void irq_init(void)
{
    ICU_MSYSINT1 = 0;
    ICU_MSYSINT2 = 0;
    ICU_MGIUINTL = 0;

    /* Clear stale cascade state, then enable GIU line 0 end to end. */
    KJ_INTACK = 0xFFFFu;
    GIU_INTSTATL = 0x0001u;
    GIU_INTENL = GIU_INTENL | 0x0001u;
    ICU_MGIUINTL = 0x0001u;
    ICU_MSYSINT1 = SYSINT1_GIU;
}

void irq_dispatch_ip2(void)
{
    uint16_t sys = ICU_SYSINT1 & ICU_MSYSINT1;

    if (!(sys & SYSINT1_GIU))
        return;
    if (ICU_GIUINTL & 0x0001u) {
        uint32_t pending = KJ_INTSTAT;

        /* CF state changes are only cleared by reading the cause register
         * (doc: CF; Linux irq.c). The NE2000 holds the line while its own
         * ISR has unmasked bits, which ne2000_irq() clears. */
        if (pending & KJ_INT_CF)
            (void)KJ_CF_CAUSE;
        KJ_INTACK = pending;
        GIU_INTSTATL = 0x0001u;
        input_irq((uint16_t)pending);
        if (pending & KJ_INT_CF)
            ne2000_irq();
    }
}
