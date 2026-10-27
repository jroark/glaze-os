/* Glaze kernel main. */

#include "kernel.h"
#include "hw.h"
#include "mips.h"
#include "proc.h"
#include "drivers.h"
#include <glaze/bootinfo.h>

/* First user process: the compositor/SpringBoard once it exists. */
#ifndef GLAZE_INIT
#define GLAZE_INIT "bin/init"
#endif

extern uint8_t __kernel_end[];

static struct gz_bootinfo boot_copy;
const struct gz_bootinfo *ram_boot;

/* GlazeBoot's block (glaze/bootinfo.h), checked before anything trusts it:
 * the romfs and the RAM disk must lie in SDRAM above the kernel, apart. */
static void take_bootinfo(const struct gz_bootinfo *bi)
{
    uint32_t low = ALIGN_UP(PHYS(__kernel_end), PAGE_SIZE);

    if (!bi)
        return;
    if ((uintptr_t)bi != KSEG0(GZ_BOOTINFO_PA) || bi->magic != GZ_BOOTINFO_MAGIC ||
        bi->version != GZ_BOOTINFO_VERSION) {
        kputs("boot: bad boot info, ignored\n");
        return;
    }
    uint32_t romfs_end = bi->romfs_pa + ALIGN_UP(bi->romfs_size, PAGE_SIZE);
    uint32_t ramfs_end = bi->ramfs_pa + bi->ramfs_blocks * NAND_PAGES_PER_BLOCK * NAND_PAGE_DATA;
    if (bi->romfs_pa % 0x2000u || bi->romfs_pa < low || romfs_end < bi->romfs_pa ||
        romfs_end > SDRAM_SIZE || bi->ramfs_pa % PAGE_SIZE || bi->ramfs_pa < low ||
        bi->ramfs_blocks < 8 || bi->ramfs_blocks > GZ_FLASH_BLOCKS ||
        ramfs_end > SDRAM_SIZE || (bi->ramfs_pa < romfs_end && bi->romfs_pa < ramfs_end))
        panic("boot: bad RAM boot layout");
    boot_copy = *bi;
    ram_boot = &boot_copy;
}

void kmain(const struct gz_bootinfo *bi)
{
    /* Disarm the PMU HALTimer watchdog (VR4131 UM §12.2.2): a single
     * HALTIMERRST write tells the PMU that software has started. */
    PMU_CNT = PMU_CNT | PMUCNT_HALTIMERRST;   /* keep what the boot ROM or Windows CE set */

    /* A RAM boot paints the boot trace (glaze/bootinfo.h) until the
     * console is known to work. */
    bool trace = bi != NULL;
#define TRACE(n, color) do { if (trace) gz_boot_trace(n, color); } while (0)
    TRACE(6, GZ_TRACE_MAGENTA);
    if (trace)
        console_ram_boot();
    kprintf("\nGlaze %s booting (PRId 0x%x, Config 0x%x, PMUINT 0x%x)\n",
            GLAZE_BUILD, read_c0_prid(), read_c0_config(), PMU_INT);
    take_bootinfo(bi);
    if (ram_boot)
        kprintf("boot: from RAM: romfs %u KiB at 0x%x, RAM disk %u KiB at 0x%x, NAND %s\n",
                ram_boot->romfs_size / 1024, ram_boot->romfs_pa, ram_boot->ramfs_blocks * 16,
                ram_boot->ramfs_pa, ram_boot->flags & GZ_BOOT_NAND_OFF ? "off" :
                ram_boot->flags & GZ_BOOT_NAND_RW ? "read/write" : "read-only");
    TRACE(console_dead() ? 11 : 7, console_dead() ? GZ_TRACE_RED : GZ_TRACE_ORANGE);

    trap_init();
    mm_init();
    TRACE(8, GZ_TRACE_GRAY);
    romfs_init();               /* reads the archive's table from NAND or RAM */
    TRACE(9, GZ_TRACE_PINK);
    irq_init();
    TRACE(12, GZ_TRACE_WHITE);
    timer_init();
    display_init(ram_boot != NULL);
    cmcu_init();
    TRACE(13, timer_rtc_ok() ? GZ_TRACE_WHITE : GZ_TRACE_RED);
    rtc_init();
    TRACE(14, GZ_TRACE_WHITE);
    input_init();
    TRACE(15, GZ_TRACE_WHITE);
    net_init();
    TRACE(16, GZ_TRACE_WHITE);
    cf_init();
    TRACE(17, GZ_TRACE_WHITE);
    audio_init();
    TRACE(18, GZ_TRACE_WHITE);
    ipc_init();
    sched_init();
    TRACE(19, GZ_TRACE_WHITE);
    romfs_list();
    TRACE(20, GZ_TRACE_WHITE);

    /* The file server owns the flash above the kernel (in a RAM boot, the
     * RAM disk and the whole NAND). */
    if (!proc_spawn("bin/fs", 0, PROC_FLASH))
        kputs("fs: no file server\n");
    TRACE(21, GZ_TRACE_WHITE);
    if (!proc_spawn("bin/net", 0, PROC_NET))
        kputs("net: no network server\n");
    TRACE(22, GZ_TRACE_WHITE);
    if (!proc_spawn("bin/audio", 0, PROC_AUDIO))
        kputs("audio: no audio server\n");
    TRACE(23, GZ_TRACE_WHITE);
    if (!proc_spawn(GLAZE_INIT, 0, PROC_PRIVILEGED))
        panic("cannot start %s", GLAZE_INIT);
    TRACE(10, GZ_TRACE_DKGREEN);
#undef TRACE

    sched_start();
}
