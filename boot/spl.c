/*
 * Stage-1 SPL for the Casio BE-300.
 *
 * Loaded into SDRAM at 0x80F00000 by the boot ROM's record walker after
 * parsing the partition-1 B000FF container. Applies an armed OTA journal
 * (boot/ota_apply.c, docs/os/OTA.md), then reads the kernel slot's header
 * (struct gz_kernel_header, glaze/ota.h) at GZ_KERNEL_NAND_OFFSET, loads the
 * flat image after it, checks its CRC and jumps to the kernel entry. The
 * SPL isn't built against a particular kernel: OTA replaces the kernel and
 * romfs but never this.
 */

#include <stdint.h>
#include <glaze/ota.h>
#include "../kernel/drivers/nand_boot.h"
#include "../kernel/resume.h"
#include "ota_apply.h"

#define NAND_PAGE_DATA      512u

/* Register overrides allow the production loader to run against a strict
 * host MMIO model (tests/native/spl_io.c). */
#ifndef SPL_REG32
#define SPL_REG32(a) (*(volatile uint32_t *)(a))
#define SPL_REG8(a)  (*(volatile uint8_t *)(a))
#endif
#ifndef SPL_REG16
#define SPL_REG16(a) (*(volatile uint16_t *)(a))
#endif
#define XFER_CMD     SPL_REG32(0xAA00A414u)
#define XFER_ADDR8   SPL_REG8(0xAA00A420u)
#define XFER_ACK     SPL_REG32(0xAA00A430u)
#define XFER_STATUS  SPL_REG32(0xAA00A440u)
#define XFER_KICK    SPL_REG32(0xAA00A460u)
#define XFER_MODE    SPL_REG32(0xAA00A464u)
#define XFER_DATA32  SPL_REG32(0xAA00A420u)
#define STREAM_DATA  SPL_REG32(0xAA00B000u)
#define STREAM_BYTE  SPL_REG8(0xAA00B000u)
#define SIU_THR      SPL_REG8(0xAA008680u)
#define SIU_LSR      SPL_REG8(0xAA008694u)
#define BLG_CONFIG   SPL_REG32(0xAA000984u)
#define BUZZ_CONTROL SPL_REG32(0xAA0009A0u)
#define BUZZ_PERIOD  SPL_REG32(0xAA0009A4u)
#define BUZZ_DUTY    SPL_REG32(0xAA0009A8u)
#define CMM_GATE     SPL_REG32(0xAA001128u)
#define CMCU_TIMEOUT SPL_REG32(0xAA007814u)
#define CMCU_STATUS  SPL_REG32(0xAA007834u)
#ifndef SPL_COUNT
#define SPL_COUNT() ({ uint32_t c_; __asm__ volatile("mfc0 %0, $9" : "=r"(c_)); c_; })
#endif

/*
 * Piezo beeps (BLG channel 09A0, docs/hardware/BUZZER.md), the only signal a NAND
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
 * 3 ambiguous boot block map, 4 kernel header or CRC wrong, 5 an OTA
 * update failed part-way (the next boot tries again). Never returns: a
 * partly loaded kernel must not run. */
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
 * catch (docs/hardware/NAND_BOOT_AUDIT.md, "Power-on findings"). Which of the gates
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
 * path turns on only 111C, 1120, 1134 and 1104 (docs/hardware/NAND_ROM_HANDOFF.md).
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

/* Boot splash (boot/splash.h): the SPL lights the panel as Casio's SPL does
 * for its own splash (boot/lcd_start.h), after the gates are on, and fills
 * the first half of the bar: the block-map scan, then the kernel load. */
#define DISP_BL_GATE        SPL_REG32(0xAA001128u)
#define DISP_LCD_CLOCK      SPL_REG32(0xAA001100u)
#define DISP_LCD_GATE       SPL_REG32(0xAA00110Cu)
#define DISP_LCD(offset)    SPL_REG32(0xAA000C00u + (offset))
#define DISP_BL_CONTROL     SPL_REG32(0xAA000980u)
#define DISP_BL_CONFIG      SPL_REG32(0xAA000984u)
#define DISP_BL_PERIOD      SPL_REG32(0xAA000988u)
#define DISP_BACKLIGHT      SPL_REG32(0xAA00098Cu)
#define DISP_CONTRAST       SPL_REG32(0xAA000C4Cu)
#ifndef SPL_FB
#define SPL_FB ((volatile uint16_t *)(uintptr_t)0xAA200000u)
#endif

static void display_delay_us(uint32_t us)
{
    uint32_t t0 = SPL_COUNT();
    while (SPL_COUNT() - t0 <= us * 10u)
        ;
}

#include "lcd_start.h"
#include "splash.h"

#define SPLASH_SCAN_READS   5120u   /* nand_boot_scan: up to 5 pages a block */
static unsigned splash_reads;
static bool splash_scanning;

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

/* Programs a page's data and all 16 spare bytes, as the kernel does
 * (kernel/drivers/nand.c): SEQIN, MODE 7 for 520 bytes, MODE 6 for the last
 * 8, PAGEPROG, then the chip's status (70h) until ready. OTA only. */
static uint8_t nand_chip_status(void)
{
    uint8_t status = 0;

    for (uint32_t n = 0; n < 100000u; n++) {
        nand_command(0x70);
        status = (uint8_t)XFER_DATA32;
        if (status & 0x40u)
            break;
    }
    return status;
}

static void nand_address_row(uint32_t page)
{
    XFER_ADDR8 = (uint8_t)page;
    XFER_ADDR8 = (uint8_t)(page >> 8);
    (void)XFER_ACK;
    XFER_CMD = 2;
    nand_wait_ready();
}

static int spl_program_raw(uint32_t page, const uint8_t raw[528])
{
    nand_select();
    nand_command(0);
    nand_command(0x80);
    XFER_CMD = 6;
    XFER_ADDR8 = 0;
    nand_address_row(page);
    XFER_KICK = 1;
    XFER_MODE = 7;
    nand_wait_ready();
    for (uint32_t i = 0; i < 520u; i++)
        STREAM_BYTE = raw[i];
    XFER_MODE = 6;
    nand_wait_ready();
    for (uint32_t i = 520u; i < 528u; i++)
        STREAM_BYTE = raw[i];
    nand_command(0x10);
    uint8_t status = nand_chip_status();
    XFER_MODE = 0;
    nand_deselect();
    return (status & 0xC1u) == 0xC0u ? 0 : -1;
}

static int spl_erase(uint32_t block)
{
    nand_select();
    nand_command(0x60);
    XFER_CMD = 6;
    nand_address_row(block * 32u);
    nand_command(0xD0);
    uint8_t status = nand_chip_status();
    nand_deselect();
    return (status & 0xC1u) == 0xC0u ? 0 : -1;
}

static struct nand_boot_map boot_map;

static int boot_raw(void *ctx, uint32_t page, uint8_t raw[528])
{
    (void)ctx;
    /* The scan fills 3-30% of the bar. */
    if (splash_scanning && !(++splash_reads & 63u))
        splash_progress(SPL_FB, 30u + (splash_reads < SPLASH_SCAN_READS ?
            splash_reads : SPLASH_SCAN_READS) * 270u / SPLASH_SCAN_READS);
    nand_read_raw_page(page, (uint32_t *)raw);
    return 0;
}

static void nand_read_page(uint32_t page, uint32_t *dest)
{
    if (!nand_boot_read(&boot_map, boot_raw, 0, page, dest, true))
        return;
    spl_fail(2, "SPL: missing or corrupt NAND boot page; boot stopped\n");
}

/* The kernel slot's header page, checked: magic, format, its own CRC, and a
 * load range clear of low RAM and of this SPL. */
static struct gz_kernel_header kernel_header;

static const struct gz_kernel_header *spl_kernel_header(void)
{
    struct gz_kernel_header *h = &kernel_header;

    nand_read_page(GZ_KERNEL_NAND_OFFSET / NAND_PAGE_DATA, (uint32_t *)h);
    if (!ota_kernel_header_ok(h))
        spl_fail(4, "SPL: no valid kernel header; boot stopped\n");
    return h;
}

/* ---- OTA (boot/ota_apply.c) ------------------------------------------------ */

static struct gz_spl_info *const spl_info = (struct gz_spl_info *)(0xA0000000u | GZ_SPL_INFO_PA);

static int ota_read_raw(uint32_t page, uint8_t raw[528])
{
    nand_read_raw_page(page, (uint32_t *)raw);
    return 0;
}

/*
 * An update takes minutes, and Casio's power-management microcontroller
 * resets the device 150 s after its watchdog was last reloaded
 * (kernel/drivers/cmcu.c). Reload it as cmcu_tick() does, every 5 s by CP0
 * Count, after the companion gates the kernel has on when it does the same.
 */
static uint32_t ota_reloaded;

static void ota_progress(uint32_t done, uint32_t total)
{
    if (SPL_COUNT() - ota_reloaded > 5000u * 10000u) {
        (void)CMCU_STATUS;
        CMCU_TIMEOUT = 0x96u;
        ota_reloaded = SPL_COUNT();
    }
    splash_ota(SPL_FB, done * 1000u / total);
}

static const struct ota_nand ota_io = {
    .read_raw = ota_read_raw,
    .program_raw = spl_program_raw,
    .erase = spl_erase,
    .progress = ota_progress,
};

static void spl_ota(void)
{
    spl_info->magic = GZ_SPL_INFO_MAGIC;
    spl_info->version = GZ_SPL_VERSION;
    spl_info->features = GZ_SPL_F_OTA;
    if (boot_map.journal == NAND_BOOT_NONE) {
        spl_info->ota_result = GZ_OTA_NONE;
        return;
    }
    uart_puts("SPL: installing an OTA update\n");
    ota_reloaded = SPL_COUNT();
    switch (ota_apply(&ota_io, &boot_map, spl_info)) {
    case OTA_APPLY_DONE:
        uart_puts("SPL: update installed\n");
        /* Scan again: blocks may have moved, and the journal is gone. */
        if (nand_boot_scan(&boot_map, boot_raw, 0, true))
            spl_fail(3, "SPL: ambiguous NAND boot block map; boot stopped\n");
        break;
    case OTA_APPLY_REJECTED:
        uart_puts("SPL: update rejected, reason 0x");
        uart_puthex32(spl_info->ota_detail);
        uart_puts("; booting the installed OS\n");
        break;
    case OTA_APPLY_FAILED:
        splash_ota_failed(SPL_FB);
        spl_fail(5, "SPL: update failed part-way; it resumes on the next boot\n");
    }
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

static void __attribute__((noreturn)) ramboot(const struct gz_kernel_header *kh)
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
    uint32_t kernel_pages = (kh->size + GB_PAGE - 1) / GB_PAGE;
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
        rb_read(rb_src(n), GZ_KERNEL_IMAGE_OFFSET + i * GB_PAGE);
        *gb_move_at(c, n) = (struct gb_move){ (uint16_t)rb_src(n),
            (uint16_t)(((kh->load & 0x1FFFFFFFu) >> 12) + i), 0, 0 };
    }
    for (uint32_t i = 0; i < romfs_pages; i++, n++) {
        rb_read(rb_src(n), RB_ROMFS_SECTOR * NAND_PAGE_DATA + i * GB_PAGE);
        *gb_move_at(c, n) = (struct gb_move){ (uint16_t)rb_src(n), (uint16_t)((romfs_pa >> 12) + i), 0, 0 };
    }
    c->count = n;
    c->entry = kh->entry;
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

/*
 * A HIBERNATE wake (docs/hardware/POWER.md "Power off", kernel/resume.h) restarts
 * the CPU at the reset vector, and the boot ROM loads this SPL as after a
 * power-on. Before anything else, and before the LCD is lit, see whether
 * the kernel is still in RAM waiting to resume:
 *   - no reset cause in PMUINT (a power-switch or alarm wake sets none,
 *     VR4131 UM 12.2.1; Power+Reset, a battery pull or the HALTimer do),
 *   - the record's magic.
 * Then Power held resumes it, as does the alarm it set; any other wake
 * hibernates again, as Windows CE's NK does (nk.exe 0x80077820-0x80077944).
 * To resume, the low RAM the boot ROM overwrote comes back from the
 * kernel's copies (kernel/resume.h), and the kernel's text and read-only
 * data must then sum as they did at build. Otherwise the record is cleared
 * and this is an ordinary boot.
 */
#define PMU_INT16       SPL_REG16(0xAF0000C0u)
#define PMU_CNT16       SPL_REG16(0xAF0000C2u)
#define RTC_ETIME16(o)  SPL_REG16(0xAF000100u + (o))
#define KEYS_LATCH16    SPL_REG16(0xAA00A042u)
#define KEYS_WORD16     SPL_REG16(0xAA00A044u)
#define PMU_RESETS      0x0138u     /* RSTSW, RTCRST, TIMOUTRST, BATTINH */
#ifndef SPL_RESUME_RECORD
#define SPL_RESUME_RECORD ((struct gz_resume *)(0xA0000000u | GZ_RESUME_PA))
#define SPL_KERNEL_TEXT(va) ((const volatile uint32_t *)((va) | 0x20000000u))
#define SPL_SAVED_PAGE(pa) ((volatile uint32_t *)(0xA0000000u | (pa)))
#define SPL_HIBERNATE() __asm__ volatile(".word 0x42000023\n\tnop\n\tnop\n\tnop\n\tnop" ::: "memory")
#define SPL_RESUME(r) ((void (*)(struct gz_resume *))((r)->entry | 0x20000000u))(r)
#endif

static uint64_t spl_etime(void)
{
    uint16_t h, m, l, m2;

    do {
        h = RTC_ETIME16(4);
        m = RTC_ETIME16(2);
        l = RTC_ETIME16(0);
        m2 = RTC_ETIME16(2);
    } while (m != m2);
    return ((uint64_t)h << 32) | ((uint32_t)m << 16) | l;
}

/* Why a hibernation wasn't resumed, for the log once serial is up. */
static const char *spl_resume_failed;
static uint32_t spl_resume_value;

static void spl_resume(void)
{
    struct gz_resume *r = SPL_RESUME_RECORD;
    uint16_t pmuint = PMU_INT16;

    if (r->magic != GZ_RESUME_MAGIC)
        return;
    if (pmuint & PMU_RESETS) {
        spl_resume_failed = "a reset, PMUINT 0x";
        spl_resume_value = pmuint;
        r->magic = 0;
        return;
    }
    r->pmuint = pmuint;
    uint64_t alarm = (uint64_t)r->alarm_hi << 32 | r->alarm_lo;
    uint32_t wake = 1;          /* GZ_WAKE_POWER */
    (void)KEYS_LATCH16;
    if (KEYS_WORD16 & 0x8000u)
        wake = 1;
    else if (alarm && spl_etime() >= alarm)
        wake = 2;               /* GZ_WAKE_ALARM */
    else {
        r->rests++;
        PMU_INT16 = 0xFB3Bu;    /* PMUINT_CLEAR_ALL, as the kernel left it */
        SPL_HIBERNATE();
        /* It didn't take: be awake rather than stuck. */
    }
    /* What the boot ROM overwrote, back from the kernel's copies. */
    for (uint32_t i = 0; i < r->nsaved && i < GZ_RESUME_SAVES; i++) {
        const volatile uint32_t *from = SPL_SAVED_PAGE(r->saved[i].copy);
        volatile uint32_t *to = SPL_SAVED_PAGE(r->saved[i].pa);
        for (uint32_t w = 0; w < 1024; w++)
            to[w] = from[w];
    }
    if (r->text_len == 0 || r->text_va < GZ_KERNEL_LOAD_MIN || r->text_va > GZ_KERNEL_LOAD_END ||
        r->text_len > GZ_KERNEL_LOAD_END - r->text_va) {
        spl_resume_failed = "no kernel text recorded, length 0x";
        spl_resume_value = r->text_len;
        r->magic = 0;
        return;
    }
    uint32_t sum = gz_resume_sum(SPL_KERNEL_TEXT(r->text_va), r->text_len);
    if (sum != r->text_sum) {
        spl_resume_failed = "kernel text changed, sum 0x";
        spl_resume_value = sum;
        r->magic = 0;
        return;
    }
    r->wake = wake;
    PMU_CNT16 = PMU_CNT16 | 4u; /* HALTIMERRST within its 4 s (UM 12.2.2) */
    r->magic = 0;
    SPL_RESUME(r);
}

uint32_t spl_main(void)
{
    spl_resume();               /* returns unless a hibernation resumes */
    CHECKPOINT(1);
    uart_init();            /* before the gates: serial stays off in the SPL */
    nand_init();
    CHECKPOINT(2);
    companion_gates_on();
    CHECKPOINT(3);
    nand_release_dma();
    CHECKPOINT(4);
    lcd_start();
    splash_draw(SPL_FB);
    splash_progress(SPL_FB, 30);
    splash_scanning = true;
    if (nand_boot_scan(&boot_map, boot_raw, 0, true))
        spl_fail(3, "SPL: ambiguous NAND boot block map; boot stopped\n");
    splash_scanning = false;
    CHECKPOINT(5);
    uart_puts("\nGlaze SPL\n");
    if (spl_resume_failed) {
        uart_puts("SPL: not resuming the hibernation: ");
        uart_puts(spl_resume_failed);
        uart_puthex32(spl_resume_value);
        uart_puts("\n");
    }
    spl_ota();
    const struct gz_kernel_header *kh = spl_kernel_header();
    uart_puts("kernel ");
    uart_puts(kh->version[15] ? "?" : kh->version);
    uart_puts(": load = 0x");
    uart_puthex32(kh->load);
    uart_puts(", entry = 0x");
    uart_puthex32(kh->entry);
    uart_puts(", size = 0x");
    uart_puthex32(kh->size);
    uart_puts("\n");
#ifdef SPL_RAMBOOT
    ramboot(kh);
#endif

    uint32_t pages = (kh->size + NAND_PAGE_DATA - 1u) / NAND_PAGE_DATA;
    uint32_t start_page = GZ_KERNEL_IMAGE_OFFSET / NAND_PAGE_DATA;

    /* Write through the uncached KSEG1 alias so the data is visible to the
     * cached kernel-entry fetch without an explicit cache flush. */
    uint32_t *dst = (uint32_t *)(uintptr_t)((kh->load & 0x1FFFFFFFu) | 0xA0000000u);
    uint32_t crc = 0, left = kh->size;

    for (uint32_t i = 0; i < pages; i++) {
        nand_read_page(start_page + i, dst);
        crc = ota_crc32(crc, (const volatile uint8_t *)dst, left < NAND_PAGE_DATA ? left : NAND_PAGE_DATA);
        left -= left < NAND_PAGE_DATA ? left : NAND_PAGE_DATA;
        dst += NAND_PAGE_DATA / 4;
        if (!(i & 15u))     /* the kernel load fills 30-50% */
            splash_progress(SPL_FB, 300u + i * 200u / pages);
    }
    if (crc != kh->image_crc)
        spl_fail(4, "SPL: kernel image CRC mismatch; boot stopped\n");
    splash_progress(SPL_FB, 500);

    uart_puts("kernel loaded; jumping\n");
    CHECKPOINT(6);
    /* Kernel entry performs its own cache setup before returning to KSEG0. */
    return kh->entry | 0x20000000u;
}
