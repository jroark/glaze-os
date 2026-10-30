/*
 * RAM boot: what GlazeBoot (wince/glazeboot), a loader run from Windows CE,
 * hands the kernel. The loader copies the kernel to its link address, the
 * romfs archive to an 8 KiB-aligned region at the top of RAM, and this
 * block to GZ_BOOTINFO_PA, then jumps to the entry with a0 = its KSEG0
 * address. The SPL passes a0 = 0, a NAND boot.
 *
 * In a RAM boot the file server's volume is a RAM disk of ramfs_blocks
 * erase blocks at ramfs_pa (lost on reset), and the whole NAND, which still
 * holds Windows CE, is the file server's through SYS_NAND_*.
 */
#ifndef GLAZE_BOOTINFO_H
#define GLAZE_BOOTINFO_H

#include <glaze/types.h>

#define GZ_BOOTINFO_MAGIC   0x42525a47u     /* "GZRB" */
#define GZ_BOOTINFO_VERSION 1u
#define GZ_BOOTINFO_PA      0xF000u         /* the page below the kernel */

#define GZ_BOOT_NAND_RW     (1u << 0)       /* SYS_NAND_PROG/ERASE allowed */
#define GZ_BOOT_NAND_OFF    (1u << 1)       /* SYS_NAND_* refused: no /NAND */
#define GZ_BOOT_NAND_REPAIR (1u << 2)       /* explicit raw repair; /NAND filesystem disabled */
#define GZ_BOOT_FROM_ROM    (1u << 3)       /* RAM root loaded by ROM, not a warm CE handoff */

struct gz_bootinfo {
    uint32_t magic, version, flags;
    uint32_t romfs_pa, romfs_size;          /* romfs_pa 8 KiB aligned */
    uint32_t ramfs_pa, ramfs_blocks;        /* 16 KiB erase blocks */
    uint32_t reserved;
};

/* The flat kernel file the loader reads (tools/mkkernelbin.py): this header
 * in a page of its own, then the image from `load` for `size` bytes. */
#define GZ_KERNEL_BIN_MAGIC 0x424b5a47u     /* "GZKB" */
#define GZ_KERNEL_BIN_HEADER 4096u

struct gz_kernel_bin {
    uint32_t magic;
    uint32_t load, entry, size;             /* KSEG0 addresses */
    uint32_t end;                           /* __kernel_end, past the BSS */
    uint32_t reserved[3];
};

/*
 * Boot trace for a RAM boot, which may hang before the console works: each
 * stage paints block n, 20 px wide and 12 rows high, 12 to a row from the
 * top left of the screen, uncached, so the last block showing after a hang
 * names the stage that stopped. The LCD scans the Casio controller's video RAM at
 * PA 0x0A200000 (kernel/hw.h FB_PA), 256 pixels to a row.
 *
 *   0 white    GlazeBoot, about to call the trampoline
 *   1 red      trampoline entered
 *   2 yellow   interrupts masked, wave DMA stopped, DMA settled
 *   3 green    caches flushed
 *   4 cyan     pages placed
 *   5 blue     caches flushed again, jumping to the kernel
 *   6 magenta  kmain entered
 *   7 orange   console set up and first line written
 *   8 gray     memory set up
 *   9 pink     romfs found
 *  10 dk green drivers up, first processes spawned
 *  11 red      the console UART never became ready, so output is off
 *
 * The second row, between 9 and 10, is white per step unless noted:
 *  12 interrupts   13 timer (red: the RTC never ticked, rate guessed)
 *  14 RTC          15 input (touch, buttons, Stowaway probe)
 *  16 NE2000       17 CompactFlash      18 audio      19 IPC, scheduler
 *  20 romfs list   21 fs spawned        22 net spawned  23 audio spawned
 *
 * During handover, third-row blocks 24 and 25 are cyan after stopping
 * USB DMA and audio DMA, respectively. SpringBoard later covers them.
 * A kernel panic paints the whole third row (24-35) red.
 */
#define GZ_TRACE_FB         0xAA200000u     /* KSEG1 of the framebuffer */
#define GZ_TRACE_STRIDE     256u
#define GZ_TRACE_WHITE      0xFFFFu
#define GZ_TRACE_RED        0xF800u
#define GZ_TRACE_YELLOW     0xFFE0u
#define GZ_TRACE_GREEN      0x07E0u
#define GZ_TRACE_CYAN       0x07FFu
#define GZ_TRACE_BLUE       0x001Fu
#define GZ_TRACE_MAGENTA    0xF81Fu
#define GZ_TRACE_ORANGE     0xFD20u
#define GZ_TRACE_GRAY       0x8410u
#define GZ_TRACE_PINK       0xFC18u
#define GZ_TRACE_DKGREEN    0x0400u

/* The framebuffer is companion memory behind the LCD clock gates (0A001100,
 * 0A00110C). A store there with either gate off stalls the bus for good: a
 * power-on recovery boot hung on its first trace write. After a CE handoff
 * or once display_init has run, both are on. */
static inline __attribute__((always_inline)) int gz_boot_trace_live(void)
{
    return (*(volatile uint32_t *)0xAA001100u & 1) && (*(volatile uint32_t *)0xAA00110Cu & 1);
}

static inline __attribute__((always_inline)) void gz_boot_trace(uint32_t n, uint16_t color)
{
    volatile uint16_t *fb = (volatile uint16_t *)GZ_TRACE_FB;

    uint32_t x0 = n % 12 * 20 + 2, y0 = n / 12 * 14;

    for (uint32_t y = y0; y < y0 + 12; y++)
        for (uint32_t x = x0; x < x0 + 16; x++)
            fb[y * GZ_TRACE_STRIDE + x] = color;
}

#endif
