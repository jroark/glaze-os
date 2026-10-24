/*
 * Stage-1 SPL for the Casio BE-300.
 *
 * Loaded into SDRAM at 0x80F00000 by the boot ROM's record walker after
 * parsing the partition-1 B000FF container. Reads the kernel image as a
 * flat binary from NAND offset KERNEL_NAND_OFFSET and jumps to the kernel
 * entry. The kernel's load VA, entry, and size are baked in by the build
 * script as compile-time constants extracted from vmlinux.
 */

#include <stdint.h>

/* Kernel layout — overridden by -DKERNEL_* on the SPL command line. */
#ifndef KERNEL_LOAD_VA
#define KERNEL_LOAD_VA      0x80020000u
#endif
#ifndef KERNEL_ENTRY_VA
#define KERNEL_ENTRY_VA     0x8020E890u
#endif
#ifndef KERNEL_SIZE
#define KERNEL_SIZE         0x30A81Cu
#endif
#ifndef KERNEL_NAND_OFFSET
#define KERNEL_NAND_OFFSET  0x14000u
#endif

#define NAND_PAGE_DATA      512u

/* VRC4173 NAND XFER engine, KSEG1 (uncached) aliases of PA 0x0A00A4xx. */
#define XFER_CMD     (*(volatile uint32_t *)0xAA00A414u)  /* phase select */
#define XFER_ADDR8   (*(volatile uint8_t  *)0xAA00A420u)  /* cmd/addr byte */
#define XFER_ACK     (*(volatile uint32_t *)0xAA00A430u)  /* commit phase */
#define XFER_STATUS  (*(volatile uint32_t *)0xAA00A440u)  /* bit 0 ready */
#define XFER_KICK    (*(volatile uint32_t *)0xAA00A460u)  /* start busy */
#define XFER_MODE    (*(volatile uint32_t *)0xAA00A464u)  /* 0x05 = 520-burst */
#define STREAM_DATA  (*(volatile uint32_t *)0xAA00B000u)  /* 4-byte stream port */

/* VRC4173 SIU (UART) at 0xAA008680, addr_mult=4. Used for early debug. */
#define SIU_THR      (*(volatile uint8_t *)0xAA008680u)        /* base + 0  */
#define SIU_LSR      (*(volatile uint8_t *)(0xAA008680u + 5*4)) /* base + 5*4 */

static void uart_putc(char c)
{
    while (!(SIU_LSR & 0x20u))
        ;
    SIU_THR = (uint8_t)c;
}

static void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s);
        s++;
    }
}

static void uart_puthex32(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4)
        uart_putc(hex[(v >> i) & 0xFu]);
}

/*
 * Read one 512-byte NAND page into `dest` (4-byte aligned). The 8 trailing
 * OOB bytes from the 520-byte stream burst are discarded. `page` is the
 * absolute page index (0-based, 32 pages/block, 1024 blocks total).
 */
static void nand_read_page(uint32_t page, uint32_t *dest)
{
    /* Command phase: READ0 (0x00) */
    XFER_CMD = 0x03u;
    XFER_ADDR8 = 0x00u;
    XFER_ACK = 0u;

    /* Address phase: 3 bytes — column_lo, page_lo, page_hi. The emulator's
     * XFER engine (`nand.c` MODE 0x05 path) decodes the address as
     * `row = addr[1] | (addr[2] << 8); col = addr[0]`, so a 4th byte would
     * overflow the 3-byte queue and shift `col` out. */
    XFER_CMD = 0x06u;
    XFER_ADDR8 = 0x00u;                              /* col_lo = 0 (start of page) */
    XFER_ADDR8 = (uint8_t)(page & 0xFFu);            /* row byte 0 (page lo) */
    XFER_ADDR8 = (uint8_t)((page >> 8) & 0xFFu);     /* row byte 1 (page hi) */
    XFER_ACK = 0u;

    /* Kick (busy = !ready), then MODE = 0x05 which the emulator's XFER
     * engine uses to latch the address into stream_page/col, mark
     * `stream_active`, and set `ready = true`. KICK before MODE matters:
     * KICK alone leaves ready=false until something else flips it, and
     * MODE=5 is the only path that activates the stream. */
    XFER_KICK = 1u;
    XFER_MODE = 0x05u;

    while (!(XFER_STATUS & 1u))
        ;

    /* 128 4-byte reads = 512 data bytes. */
    for (int i = 0; i < 128; i++)
        dest[i] = STREAM_DATA;
    /* Drain the 8 OOB bytes (2 words). */
    (void)STREAM_DATA;
    (void)STREAM_DATA;
}

#ifdef SPL_RAMBOOT
/*
 * Test build (make ramboot): hands over to the kernel the way GlazeBoot does
 * from Windows CE (wince/glazeboot), so the emulator can test the RAM boot
 * and the trampoline. The kernel and romfs pages are read into frames
 * scattered over 3-15 MiB (many inside the romfs's own destination), a
 * plan (plan.h) is built in frames 0x200 on, and GlazeBoot's trampoline
 * puts everything in place and starts the kernel with a gz_bootinfo.
 */
#include <glaze/bootinfo.h>
#include "../wince/glazeboot/plan.h"

extern char tramp_entry[], __tramp_end[];

#define RB_POOL_FIRST   0x300u      /* source frames: 3 MiB ... */
#define RB_POOL_FRAMES  0xC00u      /* ... to 15 MiB, below the SPL */
#define RB_PLAN_FIRST   0x200u      /* control, code, moves, who */
#define RB_RAMFS_LEN    (2u << 20)
#define RB_ROMFS_SECTOR (0x40000u / NAND_PAGE_DATA)
#define RB_SDRAM        (16u << 20)

static uint32_t rb_src(uint32_t i)
{
    return RB_POOL_FIRST + i * 997u % RB_POOL_FRAMES;   /* 997 is prime: no repeats */
}

static void rb_read(uint32_t frame, uint32_t nand_offset)
{
    for (uint32_t i = 0; i < GB_PAGE / NAND_PAGE_DATA; i++)
        nand_read_page(nand_offset / NAND_PAGE_DATA + i, GB_PAGE_PTR(frame) + i * NAND_PAGE_DATA / 4);
}

static void __attribute__((noreturn)) ramboot(void)
{
    static uint32_t table[NAND_PAGE_DATA / 4];
    uint32_t romfs_sectors = 0;

    nand_read_page(0, table);
    for (uint32_t i = 0; i < 32; i++)
        if (table[i * 4 + 2] == RB_ROMFS_SECTOR)
            romfs_sectors = table[i * 4 + 3];
    if (!romfs_sectors) {
        uart_puts("ramboot: no romfs partition\n");
        for (;;)
            ;
    }
    uint32_t romfs_size = romfs_sectors * NAND_PAGE_DATA;
    uint32_t romfs_pages = (romfs_size + GB_PAGE - 1) / GB_PAGE;
    uint32_t kernel_pages = (KERNEL_SIZE + GB_PAGE - 1) / GB_PAGE;
    uint32_t romfs_pa = (RB_SDRAM - romfs_pages * GB_PAGE) & ~0x1FFFu;

    struct gb_control *c = (struct gb_control *)GB_PAGE_PTR(RB_PLAN_FIRST);
    for (uint32_t i = 0; i < GB_PAGE / 4; i++)
        ((uint32_t *)c)[i] = 0;
    c->magic = GB_CONTROL_MAGIC;
    for (uint32_t i = 0; i < GB_MAX_MOVE_PAGES; i++)
        c->move_frames[i] = RB_PLAN_FIRST + 2 + i;
    for (uint32_t i = 0; i < GB_WHO_PAGES; i++)
        c->who_frames[i] = RB_PLAN_FIRST + 2 + GB_MAX_MOVE_PAGES + i;

    uint32_t n = 0;
    struct gz_bootinfo *bi = (struct gz_bootinfo *)GB_PAGE_PTR(rb_src(n));
    bi->magic = GZ_BOOTINFO_MAGIC;
    bi->version = GZ_BOOTINFO_VERSION;
    bi->flags = GZ_BOOT_NAND_RW;
    bi->romfs_pa = romfs_pa;
    bi->romfs_size = romfs_size;
    bi->ramfs_pa = romfs_pa - RB_RAMFS_LEN;
    bi->ramfs_blocks = RB_RAMFS_LEN / (16u * 1024);
    bi->reserved = 0;
    *gb_move_at(c, n) = (struct gb_move){ (uint16_t)rb_src(n), GZ_BOOTINFO_PA / GB_PAGE, 0, 0 };
    n++;
    for (uint32_t i = 0; i < kernel_pages; i++, n++) {
        rb_read(rb_src(n), KERNEL_NAND_OFFSET + i * GB_PAGE);
        *gb_move_at(c, n) = (struct gb_move){ (uint16_t)rb_src(n),
            (uint16_t)(((KERNEL_LOAD_VA & 0x1FFFFFFFu) >> 12) + i), 0, 0 };
    }
    for (uint32_t i = 0; i < romfs_pages; i++, n++) {
        rb_read(rb_src(n), RB_ROMFS_SECTOR * NAND_PAGE_DATA + i * GB_PAGE);
        *gb_move_at(c, n) = (struct gb_move){ (uint16_t)rb_src(n), (uint16_t)((romfs_pa >> 12) + i), 0, 0 };
    }
    c->count = n;
    c->entry = KERNEL_ENTRY_VA;
    c->arg = 0x80000000u | GZ_BOOTINFO_PA;

    /* Through KSEG1: the trampoline runs before it flushes the D-cache. */
    uint32_t *code = (uint32_t *)(0xA0000000u | (RB_PLAN_FIRST + 1) << 12);
    for (uint32_t i = 0; i < (uint32_t)(__tramp_end - tramp_entry + 3) / 4; i++)
        code[i] = ((const uint32_t *)tramp_entry)[i];
    uart_puts("ramboot: ");
    uart_puthex32(n);
    uart_puts(" pages scattered; romfs to 0x");
    uart_puthex32(romfs_pa);
    uart_puts("; trampoline\n");
    ((void (*)(uint32_t))(0x80000000u | (RB_PLAN_FIRST + 1) << 12))((uint32_t)c);
    for (;;)
        ;
}
#endif

uint32_t spl_main(void)
{
    uart_puts("\nGlaze SPL\n");
    uart_puts("kernel load = 0x");
    uart_puthex32(KERNEL_LOAD_VA);
    uart_puts(", entry = 0x");
    uart_puthex32(KERNEL_ENTRY_VA);
    uart_puts(", size = 0x");
    uart_puthex32(KERNEL_SIZE);
    uart_puts("\n");
#ifdef SPL_RAMBOOT
    ramboot();
#endif

    uint32_t pages = (KERNEL_SIZE + NAND_PAGE_DATA - 1u) / NAND_PAGE_DATA;
    uint32_t start_page = KERNEL_NAND_OFFSET / NAND_PAGE_DATA;

    /* Write through the uncached KSEG1 alias so the data is visible to the
     * cached kernel-entry fetch without an explicit cache flush. */
    uint32_t *dst = (uint32_t *)((KERNEL_LOAD_VA & 0x1FFFFFFFu) | 0xA0000000u);

    for (uint32_t i = 0; i < pages; i++) {
        nand_read_page(start_page + i, dst);
        dst += NAND_PAGE_DATA / 4;
    }

    uart_puts("kernel loaded; jumping\n");
    return KERNEL_ENTRY_VA;
}
