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
#include "../kernel/drivers/nand_boot.h"

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

/* Register overrides allow the production loader to run against a strict
 * host MMIO model (tests/spl_io.c). */
#ifndef SPL_REG32
#define SPL_REG32(a) (*(volatile uint32_t *)(a))
#define SPL_REG8(a)  (*(volatile uint8_t *)(a))
#endif
#define XFER_CMD     SPL_REG32(0xAA00A414u)
#define XFER_ADDR8   SPL_REG8(0xAA00A420u)
#define XFER_ACK     SPL_REG32(0xAA00A430u)
#define XFER_STATUS  SPL_REG32(0xAA00A440u)
#define XFER_KICK    SPL_REG32(0xAA00A460u)
#define XFER_MODE    SPL_REG32(0xAA00A464u)
#define STREAM_DATA  SPL_REG32(0xAA00B000u)
#define SIU_THR      SPL_REG8(0xAA008680u)
#define SIU_LSR      SPL_REG8(0xAA008694u)

static uint32_t uart_dead;

/* After a power-on the companion SIU's clock gate is off, and any access to
 * a gated companion unit stalls the bus for good, which no poll bound can
 * catch (docs/NAND_BOOT_AUDIT.md, "Power-on findings"). Which of the gates
 * Windows CE leaves on clocks the SIU is still open, so the SPL stays silent
 * unless all of them are on; the kernel turns them on itself. */
static void uart_init(void)
{
    static const uint16_t gates[] = { 0x1108, 0x112C, 0x1138, 0x113C };

    for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++)
        if (!(SPL_REG32(0xAA000000u + gates[i]) & 1u))
            uart_dead = 1;
}

static void uart_putc(char c)
{
    /* Serial is optional. A gated UART must not prevent loading the OS. */
    if (uart_dead)
        return;
    for (uint32_t n = 0; n < 200000u; n++) {
        if (SIU_LSR & 0x20u) {
            SIU_THR = (uint8_t)c;
            return;
        }
    }
    uart_dead = 1;
}

static void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

static void uart_puthex32(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4)
        uart_putc(hex[(v >> i) & 0xFu]);
}

static void nand_deselect(void)
{
    XFER_CMD = 0;
    SPL_REG32(0xAA00A410u) = 0;
}

static void nand_wait_ready(void)
{
    for (uint32_t n = 0; n < 1000000u; n++)
        if (XFER_STATUS & 1u)
            return;
    nand_deselect();
    uart_puts("SPL: NAND timeout; boot stopped\n");
#ifdef SPL_TEST_STOP
    SPL_TEST_STOP();
#endif
    for (;;) ;  /* Never jump to a partially loaded kernel. */
}

static void nand_select(void)
{
    SPL_REG32(0xAA001134u) = 1; /* NAND unit clock, before engine access */
    SPL_REG32(0xAA00A400u) = 1;
    SPL_REG32(0xAA00A408u) = 1;
    SPL_REG32(0xAA00A40Cu) = 1;
    SPL_REG32(0xAA00A410u) = 1;
}

static void nand_command(uint8_t cmd)
{
    XFER_CMD = 2;
    XFER_CMD = 3;
    XFER_ADDR8 = cmd;
    (void)XFER_ACK;
    XFER_CMD = 2;
    nand_wait_ready();
}

static void nand_init(void)
{
    /* Reclaim PIO from ROM/CE DMA, as kernel/drivers/nand.c does. */
    SPL_REG32(0xAA001134u) = 1;
    SPL_REG32(0xAA001CF4u) = 1;
    SPL_REG32(0xAA001CC0u) = 0;
    SPL_REG32(0xAA00A400u) = 0;
    XFER_MODE = 0;
    nand_deselect();
    nand_select();
    nand_command(0xFF);
    nand_deselect();
}

/* Use the hardware-tested kernel PIO framing, including read ACK and all
 * 16 spare bytes. Do not depend on the ROM leaving the engine selected. */
static void nand_read_raw_page(uint32_t page, uint32_t *dest)
{
    nand_select();
    nand_command(0);
    XFER_CMD = 6;
    XFER_ADDR8 = 0;
    XFER_ADDR8 = (uint8_t)page;
    XFER_ADDR8 = (uint8_t)(page >> 8);
    (void)XFER_ACK;
    XFER_CMD = 2;
    nand_wait_ready();
    XFER_KICK = 1;
    XFER_MODE = 5;
    nand_wait_ready();
    for (uint32_t i = 0; i < 528u / 4; i++)
        dest[i] = STREAM_DATA;
    XFER_MODE = 0;
    nand_deselect();
}

static struct nand_boot_map boot_map;

static int boot_raw(void *ctx, uint32_t page, uint8_t raw[528])
{
    (void)ctx;
    nand_read_raw_page(page, (uint32_t *)raw);
    return 0;
}

static void nand_read_page(uint32_t page, uint32_t *dest)
{
    if (!nand_boot_read(&boot_map, boot_raw, 0, page, dest, true))
        return;
    uart_puts("SPL: missing or corrupt NAND boot page; boot stopped\n");
#ifdef SPL_TEST_STOP
    SPL_TEST_STOP();
#endif
    for (;;) ;
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
    uart_init();
    nand_init();
    if (nand_boot_scan(&boot_map, boot_raw, 0, true)) {
        uart_puts("SPL: ambiguous NAND boot block map; boot stopped\n");
        for (;;) ;
    }
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
    /* Kernel entry performs its own cache setup before returning to KSEG0. */
    return KERNEL_ENTRY_VA | 0x20000000u;
}
