/* Return a RAM-booted Glaze device to the stock boot ROM. No NAND writes. */
#include "kernel.h"
#ifndef REBOOT_TEST
#include "hw.h"
#endif
#include "drivers.h"

int sys_boot_mode(void)
{
    return ram_boot ? GZ_BOOT_MODE_CE : GZ_BOOT_MODE_NAND;
}

int sys_reboot_ce(struct proc *p)
{
    if (!(p->flags & PROC_PRIVILEGED) || !ram_boot) return GZ_EPERM;
    machine_reboot_ce();
}

#ifndef REBOOT_TEST
extern const uint8_t reboot_entry[], reboot_entry_end[];

void machine_reboot_ce(void)
{
    /* Low RAM is reserved by mm_init. The reset stub executes uncached at
     * PA 1000, outside the vectors/mailboxes and the kernel being erased. */
    uint32_t size = (uint32_t)(reboot_entry_end - reboot_entry);
    KASSERT(size <= PAGE_SIZE);
    void *code = (void *)KSEG0(0x1000u);
    memset(code, 0, PAGE_SIZE);
    memcpy(code, reboot_entry, size);
    mm_sync_code(code, PAGE_SIZE);

    usb_input_cancel();
    usb_stop();
    ICU_MSYSINT1 = ICU_MSYSINT2 = ICU_MGIUINTL = ICU_MGIUINTH = 0;
    AUD_ARM = 0;
    BUZZ_CONTROL = 0;
    AUD_CTRL &= ~AUD_CTRL_OUT;
    AUD_DMA_RUN &= ~1u;
    AUD_START &= ~2u;
    AUD_ACK0 = AUD_ACK1 = 0x11;
    /* nand=off may leave the unit gated. A gated unit cannot be doing DMA;
     * avoid touching its controller registers until its clock is live. */
    if (NAND_UNIT_CLOCK & 1u) NAND_DMA_ENABLE = 0;
    /* All subsequent code is uncached and uses registers only: no stack,
     * globals, scheduler, or return to memory that is about to be cleared. */
    ((void (*)(void))KSEG1(0x1000u))();
    for (;;) ;
}
#endif
