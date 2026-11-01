/* Execute production SPL reads against a NAND model that rejects gated I/O,
 * DMA ownership and incomplete page drains. No physical memory is accessed. */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static volatile uint32_t regs[0x10000 / 4];
static volatile uint8_t byte_reg;
static uint32_t pending, phase, cursor, row, stream;
static uint8_t addr[3];
static unsigned ack_pending;
static unsigned naddr, reads, acks, commands, resets;
static int stuck, serial_off;
static unsigned siu_accesses;
static jmp_buf stopped;
static uint32_t fake_count;
static unsigned buzz_starts, buzz_low, buzz_high;
static void flush(void)
{
    if (ack_pending) {
        assert(regs[0xA430 / 4] == 0x12345678);
        ack_pending = 0;
    }
    uint32_t off = pending;
    pending = 0;
    if (off == 0xA420) {
        if (phase == 6) { assert(naddr < 3); addr[naddr++] = byte_reg; }
        else { assert(phase == 3); commands++; if (byte_reg == 0xFF) resets++; }
    } else if (off == 0xA414) {
        phase = regs[off / 4];
        if (phase == 6) naddr = 0;
    } else if (off == 0x09A0 && regs[off / 4] == 1) {
        /* The piezo only with the shared gate and buzzer config on. */
        assert((regs[0x1128 / 4] & 1) && (regs[0x0984 / 4] & 0x0202) == 0x0202);
        assert(regs[0x09A8 / 4] == regs[0x09A4 / 4] / 2);
        buzz_starts++;
        if (regs[0x09A4 / 4] == 1411200 / 500 - 1) buzz_low++;
        else { assert(regs[0x09A4 / 4] == 1411200 / 1000 - 1); buzz_high++; }
    } else if (off == 0xA464) {
        if (regs[off / 4] == 4) {
            assert(stream && cursor == 130);    /* the ROM's MODE 4 point */
        } else if (regs[off / 4] == 5) {
            assert(regs[0x1134 / 4] == 1 && !regs[0x1CC0 / 4]);
            assert(regs[0xA400 / 4] && regs[0xA408 / 4]);
            assert(regs[0xA40C / 4] && regs[0xA410 / 4]);
            assert(naddr == 3 && addr[0] == 0 && regs[0xA460 / 4] == 1);
            row = addr[1] | (uint32_t)addr[2] << 8;
            assert(!stream); stream = 1; cursor = 0;
        } else if (!regs[off / 4] && stream) {
            assert(cursor == 132); stream = 0;
        }
    }
}
static volatile uint32_t *reg32(uint32_t a)
{
    flush();
    unsigned off = a & 0xffff;
    assert(off % 4 == 0);
    /* The NAND DMA word and the stream port hung the bus on a NAND boot
     * before Casio's SPL gates were on. */
    if (off == 0x1CC0 || off == 0x1CF4 || off == 0xB000)
        assert((regs[0x1128 / 4] & 1) && regs[0x1100 / 4] && regs[0x110C / 4]);
    if (off == 0xA440) regs[off / 4] = !stuck;
    else if (off == 0xA430) { acks++; regs[off / 4] = 0x12345678; ack_pending = 1; }
    else if (off == 0xB000) {
        /* MODE 5 streams 520 bytes; the last 8 spare bytes need MODE 4. */
        assert(stream && cursor < (regs[0xA464 / 4] == 4 ? 132u : 130u));
        regs[off / 4] = (row << 16) | cursor++;
        reads++;
    } else pending = off;
    return &regs[off / 4];
}
static volatile uint8_t *reg8(uint32_t a)
{
    flush();
    if ((a & 0xffff) == 0x8694 || (a & 0xffff) == 0x8680) siu_accesses++;
    if ((a & 0xffff) == 0x8694) byte_reg = serial_off ? 0 : 0x20;
    else if ((a & 0xffff) == 0xA420) pending = 0xA420;
    else assert((a & 0xffff) == 0x8680);
    return &byte_reg;
}
/* The hibernate resume (spl_resume): PMU, RTC and button halfwords, a RAM
 * model for the record, the kernel text and the saved pages, and the
 * HIBERNATE and the jump into the kernel as flags. */
static volatile uint16_t pmu_int, pmu_cnt, etime16[3], keys_latch, keys_word;
static uint16_t pmu_int_written;
static volatile uint16_t *reg16(uint32_t a)
{
    switch (a) {
    case 0xAF0000C0u: return &pmu_int;
    case 0xAF0000C2u: return &pmu_cnt;
    case 0xAF000100u: case 0xAF000102u: case 0xAF000104u: return &etime16[(a - 0xAF000100u) / 2];
    case 0xAA00A042u: return &keys_latch;
    case 0xAA00A044u: return &keys_word;
    }
    assert(!"unexpected 16-bit register");
    return NULL;
}
#define RAM_BYTES 0x60000u
static uint32_t ram[RAM_BYTES / 4];
static unsigned hibernations, resumes;
static jmp_buf resumed;
#define SPL_REG16(a) (*reg16(a))
#define SPL_RESUME_RECORD ((struct gz_resume *)&ram[GZ_RESUME_PA / 4])
#define SPL_KERNEL_TEXT(va) ((const volatile uint32_t *)&ram[((va) & 0x1FFFFFFFu) / 4])
#define SPL_SAVED_PAGE(pa) ((volatile uint32_t *)&ram[(pa) / 4])
#define SPL_HIBERNATE() (hibernations++, pmu_int_written = pmu_int)
#define SPL_RESUME(r) (resumes++, longjmp(resumed, 1))
#define SPL_REG32(a) (*reg32(a))
#define SPL_REG8(a) (*reg8(a))
#define SPL_TEST_STOP() longjmp(stopped, 1)
#define SPL_COUNT() (fake_count += 50000u)  /* 5 ms a read */
#include "../boot/spl.c"
/* OTA is tested by ota-test (tests/ota.c); here only its CRC is needed. */
int ota_apply(const struct ota_nand *io, struct nand_boot_map *map, struct gz_spl_info *info)
{
    (void)io; (void)map; info->ota_result = GZ_OTA_NONE;
    return OTA_APPLY_NONE;
}
uint32_t ota_crc32(uint32_t crc, const volatile uint8_t *p, uint32_t n)
{
    return gz_crc32(crc, (const void *)p, n);
}
bool ota_kernel_header_ok(const struct gz_kernel_header *h)
{
    return h->magic == GZ_KHDR_MAGIC;
}
/* The kernel reader must also stop before exposing a timed-out page. */
#define GLAZE_HW_H
#define NAND_PAGES_PER_BLOCK 32u
#define NAND_DMA_ENABLE SPL_REG32(0xAA001CC0u)
#define NAND_DMA_STATE SPL_REG32(0xAA001CF4u)
#define NAND_UNIT_CLOCK SPL_REG32(0xAA001134u)
#define NAND_XFER_EN0 SPL_REG32(0xAA00A400u)
#define NAND_XFER_EN2 SPL_REG32(0xAA00A408u)
#define NAND_XFER_PIO SPL_REG32(0xAA00A40Cu)
#define NAND_XFER_CTRL SPL_REG32(0xAA00A410u)
#define NAND_XFER_CMD SPL_REG32(0xAA00A414u)
#define NAND_XFER_ACK SPL_REG32(0xAA00A430u)
#define NAND_XFER_STATUS SPL_REG32(0xAA00A440u)
#define NAND_XFER_KICK SPL_REG32(0xAA00A460u)
#define NAND_XFER_MODE SPL_REG32(0xAA00A464u)
#define NAND_STREAM32 SPL_REG32(0xAA00B000u)
#define NAND_XFER_ADDR8 XFER_ADDR8
#define NAND_XFER_DATA32 SPL_REG32(0xAA00A420u)
#define NAND_STREAM8 SPL_REG8(0xAA00B000u)
#define nand_read_page kernel_nand_read_page
#include "../kernel/drivers/nand.c"
#undef nand_read_page
void panic(const char *fmt, ...) { (void)fmt; longjmp(stopped, 1); }

/* A record as power.c leaves it, with page 0x10000 (kernel text, zeros
 * here) saved to 0x50000; the boot ROM has since overwritten the original. */
static struct gz_resume *hibernated(uint32_t alarm)
{
    struct gz_resume *r = (struct gz_resume *)&ram[GZ_RESUME_PA / 4];
    memset(ram, 0, sizeof(ram));
    r->magic = GZ_RESUME_MAGIC;
    r->entry = 0x8001EB40u;
    r->alarm_lo = alarm;
    r->nsaved = 1;
    r->saved[0].pa = 0x10000;
    r->saved[0].copy = 0x50000;
    r->text_va = 0x80010000u;           /* power.c's sum of the kernel text: zeros */
    r->text_len = 0x1000u;
    r->text_sum = 0;
    ram[0x10000 / 4] = 0x00010000u;     /* the ROM's probe */
    pmu_int = 0; pmu_cnt = 0; keys_latch = keys_word = 0;
    etime16[0] = etime16[1] = etime16[2] = 0;
    hibernations = resumes = 0;
    spl_resume_failed = NULL;
    return r;
}

static bool try_resume(void)
{
    if (setjmp(resumed))
        return true;
    spl_resume();
    return false;
}

static void resume_tests(void)
{
    struct gz_resume *r = hibernated(0);
    r->magic = 0;                       /* an ordinary boot */
    assert(!try_resume() && !spl_resume_failed && ram[0x10000 / 4]);

    r = hibernated(0);                  /* Power+Reset: boot afresh */
    pmu_int = 0x0008;
    assert(!try_resume() && !r->magic && spl_resume_failed && spl_resume_value == 8);

    r = hibernated(0);                  /* Power held: put low RAM back and resume */
    keys_word = 0x8000;
    assert(try_resume() && r->wake == 1 && !r->magic && (pmu_cnt & 4) && !hibernations);
    assert(!ram[0x10000 / 4]);

    r = hibernated(5000);               /* the alarm, not yet */
    etime16[0] = 4999;
    assert(try_resume() && hibernations == 1 && r->rests == 1 && pmu_int_written == 0xFB3B);
    assert(r->wake == 1);               /* HIBERNATE didn't take: awake, not stuck */
    r = hibernated(5000);               /* the alarm, due */
    etime16[0] = 5000;
    assert(try_resume() && r->wake == 2 && !hibernations);
    r = hibernated(0x10000u);           /* the alarm across a halfword */
    etime16[1] = 1;
    assert(try_resume() && r->wake == 2);

    r = hibernated(0);                  /* a record without the kernel's text range */
    keys_word = 0x8000;
    r->text_len = 0;
    assert(!try_resume() && !r->magic && spl_resume_failed && !resumes);

    r = hibernated(0);                  /* the saved copy is no good */
    keys_word = 0x8000;
    ram[0x50000 / 4 + 7] = 1;
    assert(!try_resume() && !r->magic && spl_resume_failed && !resumes);
}

int main(void)
{
    resume_tests();
    regs[0x1CC0 / 4] = 1;   /* DMA owning the NAND at the handoff */
    nand_init(); flush();
    assert(resets == 1 && regs[0x1CC0 / 4] == 1);
    companion_gates_on(); flush();
    nand_release_dma(); flush();
    assert(resets == 2 && !regs[0x1CC0 / 4] && !regs[0x1CF4 / 4]);
    assert(!regs[0xA410 / 4]);
    uint32_t data[132];
    const uint32_t pages[] = {0, 31, 32, 255, 256, 32767};
    for (unsigned j = 0; j < sizeof(pages)/sizeof(pages[0]); j++) {
        regs[0x1134 / 4] = regs[0xA400 / 4] = regs[0xA408 / 4] = 0;
        regs[0xA40C / 4] = regs[0xA410 / 4] = 0;
        nand_read_raw_page(pages[j], data); flush();
        for (unsigned i = 0; i < 128; i++) assert(data[i] == (pages[j] << 16 | i));
        assert(!stream && !regs[0xA410 / 4] && !regs[0xA414 / 4]);
    }
    assert(reads == 6 * 132 && acks == 14 && commands == 8);
    /* A power-on leaves the SIU's gate off: no access at all, not a bounded poll. */
    regs[0x1108 / 4] = regs[0x112C / 4] = regs[0x1138 / 4] = regs[0x113C / 4] = 1;
    uart_init(); assert(!uart_dead);
    regs[0x1138 / 4] = 0;
    uart_init(); assert(uart_dead);
    regs[0x1138 / 4] = 1;
    unsigned siu_before = siu_accesses;
    uart_puts("gated serial"); assert(siu_accesses == siu_before);
    uart_dead = 0;
    serial_off = 1; uart_puts("unavailable serial"); assert(uart_dead);
    unsigned old_reads = reads;
    stuck = 1;
    /* A fatal stop beeps its code (1: timeout) three times, at power-on
     * gate state, and gives the shared gate and BLG config back. */
    regs[0x1128 / 4] = 0x11; regs[0x0984 / 4] = 0x40; uart_dead = 1;
    siu_before = siu_accesses;
    if (!setjmp(stopped)) { nand_read_raw_page(12, data); assert(!"timeout returned"); }
    flush();
    assert(reads == old_reads && !regs[0xA410 / 4]);
    assert(buzz_starts == 6 && buzz_low == 3 && buzz_high == 3);
    assert(!regs[0x09A0 / 4] && regs[0x1128 / 4] == 0x11 && regs[0x0984 / 4] == 0x40);
    assert(siu_accesses == siu_before);
    buzz_starts = buzz_low = buzz_high = 0;
    if (!setjmp(stopped)) spl_fail(3, "");
    flush();
    assert(buzz_starts == 12 && buzz_low == 3 && buzz_high == 9);
    stuck = 0;
    regs[0x1CC0 / 4] = 1;       /* stale CE DMA owner */
    nand_reset(); flush();
    assert(!regs[0x1CC0 / 4]);
    kernel_nand_read_page(256, data); flush();
    for (unsigned i = 0; i < 128; i++) assert(data[i] == (256u << 16 | i));
    old_reads = reads; stuck = 1;
    if (!setjmp(stopped)) { kernel_nand_read_page(12, data); assert(!"kernel timeout returned"); }
    flush();
    assert(reads == old_reads && !regs[0xA410 / 4]);
    puts("spl-io: NAND DMA and stream only after Casio SPL gates, kernel DMA reclaim, page boundaries, full spare drain, gated and absent serial, fatal timeout and its error beeps, hibernate resume, sleeping on and its refusals PASS");
}
