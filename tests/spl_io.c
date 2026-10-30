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
    } else if (off == 0xA464) {
        if (regs[off / 4] == 5) {
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
    if (off == 0xA440) regs[off / 4] = !stuck;
    else if (off == 0xA430) { acks++; regs[off / 4] = 0x12345678; ack_pending = 1; }
    else if (off == 0xB000) {
        assert(stream && cursor < 132);
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
#define SPL_REG32(a) (*reg32(a))
#define SPL_REG8(a) (*reg8(a))
#define SPL_TEST_STOP() longjmp(stopped, 1)
#include "../boot/spl.c"
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

int main(void)
{
    regs[0x1CC0 / 4] = 1; /* stale CE DMA owner */
    nand_init(); flush();
    assert(!regs[0x1CC0 / 4] && resets == 1);
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
    assert(reads == 6 * 132 && acks == 13 && commands == 7);
    /* A power-on leaves the SIU's gate off: no access at all, not a bounded poll. */
    regs[0x1108 / 4] = regs[0x112C / 4] = regs[0x1138 / 4] = regs[0x113C / 4] = 1;
    uart_init(); assert(!uart_dead);
    regs[0x1138 / 4] = 0;
    uart_init(); assert(uart_dead);
    unsigned siu_before = siu_accesses;
    uart_puts("gated serial"); assert(siu_accesses == siu_before);
    uart_dead = 0;
    serial_off = 1; uart_puts("unavailable serial"); assert(uart_dead);
    unsigned old_reads = reads;
    stuck = 1;
    if (!setjmp(stopped)) { nand_read_raw_page(12, data); assert(!"timeout returned"); }
    flush();
    assert(reads == old_reads && !regs[0xA410 / 4]);
    stuck = 0; nand_reset(); flush();
    kernel_nand_read_page(256, data); flush();
    for (unsigned i = 0; i < 128; i++) assert(data[i] == (256u << 16 | i));
    old_reads = reads; stuck = 1;
    if (!setjmp(stopped)) { kernel_nand_read_page(12, data); assert(!"kernel timeout returned"); }
    flush();
    assert(reads == old_reads && !regs[0xA410 / 4]);
    puts("spl-io: gated NAND/DMA reclaim, page boundaries, full spare drain, gated and absent serial, fatal timeout PASS");
}
