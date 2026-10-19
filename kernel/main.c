/* Glaze kernel main. */

#include "kernel.h"
#include "hw.h"
#include "mips.h"
#include "proc.h"
#include "drivers.h"

/* First user process: the compositor/SpringBoard once it exists. */
#ifndef GLAZE_INIT
#define GLAZE_INIT "bin/init"
#endif

void kmain(void)
{
    /* Disarm the PMU HALTimer watchdog (VR4131 UM §12.2.2): a single
     * HALTIMERRST write tells the PMU that software has started. */
    PMU_CNT = PMUCNT_HALTIMERRST;

    kprintf("\nGlaze %s booting (PRId 0x%x, PMUINT 0x%x)\n",
            GLAZE_BUILD, read_c0_prid(), PMU_INT);

    trap_init();
    mm_init();
    irq_init();
    timer_init();
    rtc_init();
    input_init();
    ne2000_init();
    audio_init();
    ipc_init();
    sched_init();
    romfs_list();

    /* The file server owns the flash above the kernel. */
    if (!proc_spawn("bin/fs", 0, PROC_FLASH))
        kputs("fs: no file server\n");
    if (!proc_spawn("bin/net", 0, PROC_NET))
        kputs("net: no network server\n");
    if (!proc_spawn("bin/audio", 0, PROC_AUDIO))
        kputs("audio: no audio server\n");
    if (!proc_spawn(GLAZE_INIT, 0, PROC_PRIVILEGED))
        panic("cannot start %s", GLAZE_INIT);

    sched_start();
}
