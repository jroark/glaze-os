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
#define BLG_CONFIG   SPL_REG32(0xAA000984u)
#define BUZZ_CONTROL SPL_REG32(0xAA0009A0u)
#define BUZZ_PERIOD  SPL_REG32(0xAA0009A4u)
#define BUZZ_DUTY    SPL_REG32(0xAA0009A8u)
#define CMM_GATE     SPL_REG32(0xAA001128u)
#ifndef SPL_COUNT
#define SPL_COUNT() ({ uint32_t c_; __asm__ volatile("mfc0 %0, $9" : "=r"(c_)); c_; })
#endif

/*
 * Piezo beeps (BLG channel 09A0, docs/BUZZER.md), the only signal a NAND
 * boot has before the kernel: the LCD is off and the SIU is gated. Gate
 * 1128 is the only gate touched, and the recovery stub beeps the same way
 * from the ROM at power-on. Timed by CP0 Count, near 10 MHz as in
 * kernel/drivers/buzzer.c. The shared gate and BLG configuration are put
 * back afterwards.
 */
static void count_wait(uint32_t ms)
{
    uint32_t t0 = SPL_COUNT();
    while (SPL_COUNT() - t0 < ms * 10000u)
        ;
}

static void spl_beeps(unsigned n, uint32_t hz, uint32_t on_ms)
{
    uint32_t gate = CMM_GATE, config = BLG_CONFIG;
    uint32_t period = 1411200u / hz - 1u;

    CMM_GATE = gate | 1u;
    BLG_CONFIG = config | 0x0202u;
    for (unsigned i = 0; i < n; i++) {
        BUZZ_PERIOD = period;
        BUZZ_DUTY = period / 2u;
        BUZZ_CONTROL = 1;
        count_wait(on_ms);
        BUZZ_CONTROL = 0;
        count_wait(180);
    }
    BLG_CONFIG = config;
    CMM_GATE = gate;
}

/* BOOT_BEEP=1 checkpoints: n short 1 kHz beeps, lower than the kernel's
 * 2 kHz groups and shorter than the recovery stub's 400 ms ones.
 *   1  the ROM started the SPL
 *   2  NAND reset (command phases only, no page data yet)
 *   3  companion gates on
 *   4  NAND DMA off, engine reset again
 *   5  boot block map read (page data through the stream port)
 *   6  kernel loaded, jumping to it (the kernel's own beeps follow) */
#ifdef SPL_BOOT_BEEP
#define CHECKPOINT(n) (spl_beeps((n), 1000u, 120u), count_wait(700))
#else
#define CHECKPOINT(n) ((void)0)
#endif

/* Fatal stops, in every build: a 1 s 500 Hz tone, then `code` short
 * beeps, three times. 1 NAND timeout, 2 missing or corrupt boot page,
 * 3 ambiguous boot block map. Never returns: a partly loaded kernel must
 * not run. */
static void uart_puts(const char *s);
static void __attribute__((noreturn)) spl_fail(unsigned code, const char *why)
{
    uart_puts(why);
    for (unsigned i = 0; i < 3; i++) {
        spl_beeps(1, 500u, 1000u);
        count_wait(300);
        spl_beeps(code, 1000u, 120u);
        count_wait(1500);
    }
#ifdef SPL_TEST_STOP
    SPL_TEST_STOP();
#endif
    for (;;) ;
}

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

/*
 * Gates on before the NAND DMA word and the first page read. The ROM's NAND
 * path turns on only 111C, 1120, 1134 and 1104 (docs/NAND_ROM_HANDOFF.md).
 * Casio's own SPL, started from the same handoff, turns on 1128, 1100 and
 * 110C (its LCD splash) before it writes 0x1CC0 and streams pages from
 * B000 (traced from the device's CE backup in the emulator). That they are
 * needed is unproven: every failed NAND boot hung at the first page read,
 * which the MODE 4 split fixed (nand_read_raw_page()). The rest are the
 * kernel's companion_gates_on() set (kernel/main.c), as Windows CE leaves
 * them. 0x0A00A0DC after 1104, as the ROM and kernel do. Gate words
 * themselves are always clocked.
 */
static void companion_gates_on(void)
{
    static const uint16_t gates[] = {
        0x1128, 0x1100, 0x110C,                         /* Casio's SPL */
        0x1104, 0x1108, 0x1120, 0x112C, 0x1138, 0x113C, /* the kernel's set */
    };

    for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++)
        SPL_REG32(0xAA000000u + gates[i]) = 1;
    SPL_REG32(0xAA00A0DCu) = 0xFF;
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
    spl_fail(1, "SPL: NAND timeout; boot stopped\n");
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
    /* Before companion_gates_on() this leaves the NAND DMA words alone:
     * see nand_release_dma(). */
    SPL_REG32(0xAA001134u) = 1;
    SPL_REG32(0xAA00A400u) = 0;
    XFER_MODE = 0;
    nand_deselect();
    nand_select();
    nand_command(0xFF);
    nand_deselect();
}

/*
 * NAND DMA off (0x1CC0 = 0, kernel/hw.h) so page data reaches the stream
 * port, then an engine reset. Casio's SPL writes only 0x1CC0, after its
 * gates and A400 = 0; 0x1CF4 is Windows CE's and is left alone here.
 */
static void nand_release_dma(void)
{
    SPL_REG32(0xAA00A400u) = 0;
    SPL_REG32(0xAA001CC0u) = 0;
    nand_init();
}

/* BOOT_BEEP=1: high 3 kHz ticks through the first page read only, so a
 * hang inside it can be placed: 1 selected, 2 READ0 sent, 3 address sent,
 * 4 burst ready, 5 first word, 6 520 bytes, 7 last spare bytes. */
#ifdef SPL_BOOT_BEEP
static unsigned reads_done;
#define TICK() do { if (!reads_done) spl_beeps(1, 3000u, 40u); } while (0)
#else
#define TICK() ((void)0)
#endif

/* Read ACK, phase close and deselect as the kernel frames them. The data
 * goes as the ROM (9FC01A98-9FC01AAE) and Casio's SPL (80F0ADFC-80F0AE28)
 * take it: 520 bytes in MODE 5, then MODE 4 for the last 8 spare bytes,
 * as programming splits MODE 7/6. Do not depend on the ROM leaving the
 * engine selected. */
static void nand_read_raw_page(uint32_t page, uint32_t *dest)
{
    nand_select();
    TICK();
    nand_command(0);
    TICK();
    XFER_CMD = 6;
    XFER_ADDR8 = 0;
    XFER_ADDR8 = (uint8_t)page;
    XFER_ADDR8 = (uint8_t)(page >> 8);
    (void)XFER_ACK;
    XFER_CMD = 2;
    nand_wait_ready();
    TICK();
    XFER_KICK = 1;
    XFER_MODE = 5;
    nand_wait_ready();
    TICK();
    dest[0] = STREAM_DATA;
    TICK();
    for (uint32_t i = 1; i < 520u / 4; i++)
        dest[i] = STREAM_DATA;
    TICK();
    XFER_MODE = 4;
    for (uint32_t i = 520u / 4; i < 528u / 4; i++)
        dest[i] = STREAM_DATA;
    TICK();
#ifdef SPL_BOOT_BEEP
    reads_done = 1;
#endif
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
    spl_fail(2, "SPL: missing or corrupt NAND boot page; boot stopped\n");
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
    CHECKPOINT(1);
    uart_init();            /* before the gates: serial stays off in the SPL */
    nand_init();
    CHECKPOINT(2);
    companion_gates_on();
    CHECKPOINT(3);
    nand_release_dma();
    CHECKPOINT(4);
    if (nand_boot_scan(&boot_map, boot_raw, 0, true))
        spl_fail(3, "SPL: ambiguous NAND boot block map; boot stopped\n");
    CHECKPOINT(5);
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
    CHECKPOINT(6);
    /* Kernel entry performs its own cache setup before returning to KSEG0. */
    return KERNEL_ENTRY_VA | 0x20000000u;
}
