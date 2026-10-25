/*
 * IP2 interrupt cascade:
 *
 *   MIPS IP2 <- VR4131 ICU SYSINT1 bit 8 (GIU) <- GIU line 0
 *            <- VRC4173 GIRQ0 status 0x0A000004: bit 1 buttons, bit 9 touch
 *
 * GIRQ0 bit 0 is the PCMCIA card, the NE2000 when one is inserted; bit 8 is
 * wave output (audio.c), whose source is cleared in its own block.
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
    ICU_MGIUINTH = 0;

    /* Only GIU line 0 is ours. After Windows CE (a RAM boot) other lines are
     * enabled, such as its serial driver's, and one firing would hold
     * SYSINT1's GIU bit with nothing here to clear it: an interrupt storm
     * on a real BE-300. Turn every other line off and clear what is
     * pending, then enable line 0 end to end (VR4131 UM ch. 14 GIU
     * GIUINTEN/GIUINTSTAT, ch. 11 ICU MGIUINTL/MGIUINTH). */
    GIU_INTENL = 0;
    GIU_INTENH = 0;
    /* Buttons are polled every tick (input_poll) and the CF disk is polled,
     * so their GIRQ0 sources stay masked; ne2000_init enables the PCMCIA
     * one for a network card. Left enabled after Windows CE, the CF card's
     * level interrupt stormed on a real BE-300. */
    KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE;
    KJ_SRC_KEYS &= ~KJ_SRC_ENABLE;
    GIU_INTSTATL = 0xFFFFu;
    GIU_INTSTATH = 0xFFFFu;
    GIU_INTENL = 0x0001u;
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

        /* As the OAL does: mask the PCMCIA source while it is handled, after
         * reading the cause register, which clears CF state changes (doc:
         * CF; Linux irq.c). The NE2000 holds the line while its own ISR has
         * unmasked bits, which netdev_irq() clears; only then is the source
         * enabled again, and only for it. */
        if (pending & KJ_INT_CF) {
            (void)KJ_CF_CAUSE;
            KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE;
        }
        if (pending & KJ_INT_KEYS)
            KJ_SRC_KEYS &= ~KJ_SRC_ENABLE;
        /* A source with no driver here, left enabled by Windows CE (a RAM
         * boot) or raised by a block it set up, would hold the line for
         * good: mask it with its enable, as the OAL masks what it serves.
         * The NAND's DMA did that on a real BE-300. */
        uint32_t foreign = pending & ~(KJ_INT_CF | KJ_INT_KEYS | KJ_INT_AUDIO | KJ_INT_TOUCH) & 0x1FFFFu;
        for (uint32_t n = 0; foreign; n++, foreign >>= 1)
            if (foreign & 1u) {
                KJ_GIRQ0_EN(n) &= ~KJ_SRC_ENABLE;
                kprintf("irq: masked GIRQ0 source %u\n", n);
            }
        GIU_INTSTATL = 0x0001u;
        input_irq((uint16_t)pending);
        /* Not while the socket is empty: a pulled network card's state
         * change keeps the line up, and enabling the source again would
         * take the interrupt forever, before pcmcia_poll() can see the
         * removal. The next card's driver enables it. */
        if (pending & KJ_INT_CF) {
            netdev_irq();
            if (netdev_present() &&
                (PCMCIA_CARD_STATUS & PCMCIA_STATUS_MASK) == PCMCIA_CARD_IN)
                KJ_SRC_PCMCIA |= KJ_SRC_ENABLE;
        }
        if (pending & KJ_INT_AUDIO)
            audio_irq();
    }
}
