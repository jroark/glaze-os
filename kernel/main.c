/* Glaze kernel main. */

#include "kernel.h"
#include "hw.h"
#include "mips.h"
#include "proc.h"
#include "drivers.h"
#include "service.h"
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

#ifdef GLAZE_HWDUMP
static uint32_t hwdump_words[2048];
#endif

/*
 * Companion clock gates that Windows CE always leaves on (be300
 * hw_dump_vrc4173.txt 0x0A001100-0x0A00113C) but no Glaze driver sets.
 * After a power-on the ROM's CF path leaves all but 111C off (HWDUMP=1
 * capture, gates 0480 with the stub's buzzer gate). Without them the first
 * console write (the SIU at 0x0A008680) stalled the bus, and with 1120 off
 * the socket status at 0x0A001000 read 0 although a card was in. 1120 is
 * followed by 1B24 = 0, 1B20 = 1 as the ROM's NAND path does (ROM
 * 9FC01D9E..1DA6, docs/NAND_ROM_HANDOFF.md). With 1104 off, USB enumerated
 * but its DMA never ran. Which unit each other gate clocks is still being
 * identified.
 */
static void companion_gates_on(void)
{
    static const uint16_t gates[] = { 0x1104, 0x1108, 0x1120, 0x112C, 0x1138, 0x113C };

    for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++)
        REG32(0x0A000000u + gates[i]) = 1;
    REG32(0x0A001B24u) = 0;
    REG32(0x0A001B20u) = 1;
    REG32(0x0A001B2Cu) = 1;     /* as Windows CE leaves it */
    REG32(0x0A00A0DCu) = 0xFF;  /* after 1104, as the ROM's NAND path does (ROM 9FC01DB4) */
    /* VR4131 GIU (UM ch. 15): Windows CE runs with GPIO4-6 as outputs,
     * GPIO4 low and GPIO5/6 high (GIUIOSELL 0x70, GIUPIODL ..E2). GPIO5 is
     * the socket supply (socket_bus_init); GPIO4 and 6 are still unnamed. */
    REG16(0x0F000144u) = (REG16(0x0F000144u) & ~0x10u) | 0x40u;
    REG16(0x0F000140u) |= 0x50u;
    for (volatile unsigned i = 0; i < 10000; i++)     /* let the units' clocks start */
        ;
}

/* BOOT_BEEP=1: n beeps at each early stage, audible with the LCD still off. */
#ifdef GLAZE_BOOT_BEEP
#define BEEP(n) buzzer_beeps(n)
#else
#define BEEP(n) do { } while (0)
#endif

void kmain(const struct gz_bootinfo *bi)
{
    BEEP(1);
#ifdef GLAZE_HWDUMP
    /* Before any store: the state the ROM, SPL or GlazeBoot handed over. */
    hwdump_setup(hwdump_words, sizeof(hwdump_words) / 4);
    hwdump_capture(0);
#endif
    BEEP(2);
    companion_gates_on();
    /* Disarm the PMU HALTimer watchdog (VR4131 UM §12.2.2): a single
     * HALTIMERRST write tells the PMU that software has started. */
    PMU_CNT = PMU_CNT | PMUCNT_HALTIMERRST;   /* keep what the boot ROM or Windows CE set */

    /* A RAM boot paints the boot trace (glaze/bootinfo.h) until the
     * console is known to work, once the framebuffer is clocked: after a
     * power-on (CF recovery) only from display_init on. */
    bool trace = bi != NULL && gz_boot_trace_live();
#define TRACE(n, color) do { if (trace) gz_boot_trace(n, color); } while (0)
    TRACE(6, GZ_TRACE_MAGENTA);
    if (bi)
        console_ram_boot();
    kprintf("\nGlaze %s booting (PRId 0x%x, Config 0x%x, PMUINT 0x%x)\n",
            GLAZE_BUILD, read_c0_prid(), read_c0_config(), PMU_INT);
    BEEP(3);
    take_bootinfo(bi);
    if (ram_boot)
        kprintf("boot: from RAM: romfs %u KiB at 0x%x, RAM disk %u KiB at 0x%x, NAND %s\n",
                ram_boot->romfs_size / 1024, ram_boot->romfs_pa, ram_boot->ramfs_blocks * 16,
                ram_boot->ramfs_pa, ram_boot->flags & GZ_BOOT_NAND_REPAIR ? "raw repair, /NAND disabled" :
                ram_boot->flags & GZ_BOOT_NAND_OFF ? "off" :
                ram_boot->flags & GZ_BOOT_NAND_RW ? "read/write" : "read-only");
    TRACE(console_dead() ? 11 : 7, console_dead() ? GZ_TRACE_RED : GZ_TRACE_ORANGE);

    trap_init();
    mm_init();
    usb_init();
    BEEP(4);
    TRACE(8, GZ_TRACE_GRAY);
    romfs_init();               /* reads the archive's table from NAND or RAM */
    TRACE(9, GZ_TRACE_PINK);
    irq_init();
    TRACE(12, GZ_TRACE_WHITE);
    BEEP(5);
    timer_init();
    BEEP(6);
    display_init(ram_boot && !(ram_boot->flags & GZ_BOOT_FROM_ROM));
    trace = bi != NULL;
    buzzer_init();
    cmcu_init();
    TRACE(13, timer_rtc_ok() ? GZ_TRACE_WHITE : GZ_TRACE_RED);
    rtc_init();
    TRACE(14, GZ_TRACE_WHITE);
    input_init();
    TRACE(15, GZ_TRACE_WHITE);
#ifdef GLAZE_COLD_SOCKET_TEST
    pcmcia_cold_init(true);
#else
    pcmcia_cold_init(false);
#endif
    net_init();
    TRACE(16, GZ_TRACE_WHITE);
    cf_init();
    TRACE(17, GZ_TRACE_WHITE);
    audio_init();
    TRACE(18, GZ_TRACE_WHITE);
#ifdef GLAZE_NAND_BOOT_PROBE
    extern void nand_boot_probe(void);
    nand_boot_probe();
#endif
    ipc_init();
    sched_init();
    TRACE(19, GZ_TRACE_WHITE);
    romfs_list();
    TRACE(20, GZ_TRACE_WHITE);

#ifdef GLAZE_HWDUMP
    hwdump_capture(1);          /* after every driver's setup */
    kputs("hwdump: captured\n");
#endif
    services_init();
    TRACE(23, GZ_TRACE_WHITE);
    if (!proc_spawn(GLAZE_INIT, 0, PROC_PRIVILEGED))
        panic("cannot start %s", GLAZE_INIT);
    TRACE(10, GZ_TRACE_DKGREEN);
#undef TRACE

    sched_start();
}
