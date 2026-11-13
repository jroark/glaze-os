/* MQ200 banked I/O model: exercise actual driver startup and complete frames,
 * not a second implementation of its initialization table. */
#define GLAZE_HW_H
#define PRESENTER_TEST
#define FB_WIDTH 240
#define FB_HEIGHT 320
static int present = 1;
#define PCMCIA_CARD_PRESENT() present
#include "../../kernel/drivers/presenter.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint16_t memory[0x800000/2];
static unsigned selected, accesses, cor_writes;
static uint16_t gate;
static uint64_t clock_us;
uint64_t time_us(void) { return clock_us; }
void presenter_cor(uint32_t address, uint8_t value)
{ assert(address == 0x200 && value == 1); cor_writes++; }
void presenter_out(unsigned port, uint16_t value)
{
    accesses++;
    if (port == 0x412) { selected = value; return; }
    if (port == 0x402) { gate = value; return; }
    assert(port < 0x400 && !(port & 1));
    unsigned address = (selected << 8) | (port & 0xff);
    assert(address < sizeof(memory)); memory[address/2] = value;
}
uint16_t presenter_in(unsigned port)
{
    accesses++; assert(port < 0x400 && !(port & 1));
    unsigned address = (selected << 8) | (port & 0xff);
    assert(address < sizeof(memory)); return memory[address/2];
}
uint16_t presenter_pixel(unsigned x, unsigned y)
{ assert(x < 240 && y < 320); return (uint16_t)(x ^ (y << 7)); }
static uint32_t reg32(unsigned address)
{ return memory[address/2] | (uint32_t)memory[address/2+1] << 16; }
int main(void)
{
    assert(presenter_init(0, 1) == GZ_EINVAL && accesses == 0);
    assert(presenter_init(0x200, 0) == GZ_EINVAL && accesses == 0);
    assert(presenter_init(0x200, 1) == GZ_ENOENT);
    assert(!enabled && gate == 0);
    memory[PCI/2] = 0x4d51; memory[PCI/2+1] = 0x0200;
    assert(presenter_init(0x200, 1) == 0 && cor_writes == 2);
    assert(reg32(GC) == 0); /* output stays blank until clear is complete */
    for (unsigned i = 0; i < 110; i++) { clock_us += 10000; presenter_update(); }
    assert(phase == 7 && reg32(GC) == 0x010200c9 && reg32(GC+4) == 0x4801);
    assert(reg32(GC+0x38) == 0x05000500);
    for (unsigned y = 0; y < HEIGHT; y++) for (unsigned x = 0; x < WIDTH; x++) {
        uint16_t expected = x >= LEFT && x < LEFT+240 && y >= TOP && y < TOP+320
            ? presenter_pixel(x-LEFT,y-TOP) : 0;
        assert(memory[y*WIDTH+x] == expected);
    }
    present = 0; unsigned before = accesses; presenter_update();
    assert(accesses == before && !enabled);
    presenter_stop(false); assert(accesses == before);
    present = 1; assert(!presenter_init(0x200,1));
    presenter_stop(true); assert(gate == 0 && reg32(GC) == 0);
    puts("presenter: startup, bank boundaries, RGB565 frame, borders and removal PASS");
}
