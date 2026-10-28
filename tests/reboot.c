/* Exercise the production boot-mode and reboot permission policy. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <glaze/bootinfo.h>
#define REBOOT_TEST
#include "../kernel/reboot.c"

const struct gz_bootinfo *ram_boot;
static jmp_buf reset;
static unsigned resets;
void machine_reboot_ce(void) { resets++; longjmp(reset, 1); }

int main(void)
{
    struct proc app = {0}, compositor = {.flags=PROC_PRIVILEGED};
    struct gz_bootinfo boot = {0};
    assert(sys_boot_mode() == GZ_BOOT_MODE_NAND);
    assert(sys_reboot_ce(&app) == GZ_EPERM);
    assert(sys_reboot_ce(&compositor) == GZ_EPERM && !resets);
    ram_boot = &boot;
    assert(sys_boot_mode() == GZ_BOOT_MODE_CE);
    assert(sys_reboot_ce(&app) == GZ_EPERM && !resets);
    /* CF/USB and reloads all carry a validated RAM boot block; USB does
     * not have to be configured for the compositor's reboot request. */
    if (!setjmp(reset)) {
        sys_reboot_ce(&compositor);
        assert(!"reboot returned");
    }
    assert(resets == 1);
    puts("reboot: NAND denied, RAM boot detected, privileged CE reset PASS");
    return 0;
}
