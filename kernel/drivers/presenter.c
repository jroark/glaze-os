/* MARGI Presenter-to-Go CF, MQ200 indirect I/O mode.
 * Register evidence: R57791.EXE MIPS ptgload.dll 0x10149c, 0x101b64;
 * PresentertoGo.dll 0x102520 (bank), 0x102600/2650 (32-bit I/O),
 * 0x1035c0-3784 (reset/power), 0x105e9c-5f64 (SDRAM),
 * 0x1048a0/49f8 (CRT). Decoded field names/timings are cross-checked
 * against NetBSD sys/arch/hpcmips/dev/mq200{reg.h,subr.c}.
 * No vendor binary or firmware is linked. See docs/hardware/PRESENTER.md.
 */
#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"

#ifdef PRESENTER_TEST
uint16_t presenter_in(unsigned);
void presenter_out(unsigned, uint16_t);
uint16_t presenter_pixel(unsigned, unsigned);
void presenter_cor(uint32_t, uint8_t);
#else
static uint16_t presenter_in(unsigned port) { return REG16(pcmcia_io_base + port); }
static void presenter_out(unsigned port, uint16_t v) { REG16(pcmcia_io_base + port) = v; }
static uint16_t presenter_pixel(unsigned x, unsigned y)
{
    return REG16(FB_PA + 2 * (y * FB_STRIDE_PX + x));
}
static void presenter_cor(uint32_t cor, uint8_t v) { REG8(pcmcia_attr_base + cor) = v; }
#endif

#define PM  0x600000u
#define CC  0x602000u
#define MM  0x604000u
#define GC  0x60a000u
#define DC  0x614000u
#define PCI 0x616000u
#define WIDTH 640u
#define HEIGHT 480u
/* Center the 240x320 portrait screen without changing its aspect ratio. */
#define LEFT ((WIDTH - FB_WIDTH) / 2)
#define TOP  ((HEIGHT - FB_HEIGHT) / 2)
static unsigned bank = ~0u, phase, row;
static uint64_t next;
static bool enabled;
static unsigned window(uint32_t address)
{
    unsigned page = address >> 8;
    if (page != bank) { presenter_out(0x412, (uint16_t)page); bank = page; }
    return address & 0x3feu;
}
static uint32_t read32(uint32_t address)
{
    unsigned p = window(address);
    /* The original driver performs two dummy reads before sampling. */
    (void)presenter_in(p); (void)presenter_in(p + 2);
    uint32_t lo = presenter_in(p);
    return lo | (uint32_t)presenter_in(p + 2) << 16;
}
static void write32(uint32_t address, uint32_t value)
{
    unsigned p = window(address);
    presenter_out(p, (uint16_t)value); presenter_out(p + 2, (uint16_t)(value >> 16));
    bank = ~0u; /* resets may discard the card's bank latch */
}
static void pixel(uint32_t address, uint16_t value)
{
    presenter_out(window(address), value);
}
int presenter_init(uint32_t cor, unsigned index)
{
    phase = row = 0; enabled = false; bank = ~0u;
    if (!cor || (cor & 1) || cor >= 0x1000 || index == 0 || index > 63) return GZ_EINVAL;
    presenter_cor(cor, (uint8_t)index);
    /* ptgload.dll enables its allocated I/O window at offset 402h. */
    presenter_out(0x402, 0x83);
    if (read32(PCI) != 0x02004d51u) { presenter_out(0x402, 0); return GZ_ENOENT; }
    enabled = true;
    write32(CC, 2); write32(CC, 0);
    write32(DC, 0x00ebac2b);  /* board oscillator/PLL1, vendor init */
    uint32_t size = read32(DC + 4) & 0x3f;
    write32(PM, (size == 4 ? 0x007c0804u : size == 8 ? 0x003c0804u : 0x804u) | 0x100);
    write32(PM + 4, 0x00050271); write32(PM + 8, 0x71);
    write32(PCI + 0x44, read32(PCI + 0x44) & ~3u);
    phase = 1; next = time_us() + 10000u;
    return 0;
}
void presenter_stop(bool present)
{
    if (enabled && present) {
        write32(GC, 0); write32(GC + 4, 0);
        presenter_out(0x402, 0);
    }
    enabled = false; phase = 0; bank = ~0u;
}
void presenter_update(void)
{
    if (!enabled || time_us() < next) return;
    if (!PCMCIA_CARD_PRESENT()) { presenter_stop(false); return; }
    if (phase == 1) {
        if (read32(PCI + 0x44) & 3) { presenter_stop(true); return; }
        write32(GC, 0); write32(GC + 4, 0);
        write32(MM, 4); next = time_us() + 10000; phase++; return;
    }
    if (phase == 2) { write32(MM, 0); next = time_us() + 5000; phase++; return; }
    if (phase == 3) {
        write32(MM + 4, 0x40615084); write32(MM + 8, 0x6d1556ff);
        write32(MM + 0x10, 0x10d); write32(MM + 0xc, 1);
        next = time_us() + 10000; phase++; return;
    }
    if (phase == 4) { write32(MM, 3); next = time_us() + 10000; phase++; return; }
    if (phase == 5) {
        /* 14.318182 MHz reference, M=211,N=15,P=3: 25.173864 MHz.
         * MQ200 PLL2 drives CRT GC1, direct RGB565, 1280 bytes/line. */
        write32(PM + 0x18, 0x00d20e30);
        write32(GC + 0x20, (WIDTH - 1) << 16);
        write32(GC + 0x24, (HEIGHT - 1) << 16);
        write32(GC + 8, 640u << 16 | 798);
        write32(GC + 0xc, 479u << 16 | 524);
        write32(GC + 0x10, 752u << 16 | 656);
        write32(GC + 0x14, 492u << 16 | 490);
        write32(GC + 0x30, 0); write32(GC + 0x38, 1280u << 16 | 1280);
        write32(GC + 0x44, 0);
        phase++; row = 0;
    }
    /* Bounded work in the service worker, never from the timer IRQ. First
     * clear the VGA surface while blanked; subsequent passes copy only the
     * LCD rectangle. The device's stride (256) is not its width (240).
     * Yield after at most eight rows / ~2 ms, keeping touch/audio responsive. */
    uint64_t until = time_us() + 2000;
    for (unsigned n = 0; n < 8; n++) {
        if (phase == 6) {
            for (unsigned x = 0; x < WIDTH; x++) pixel(2 * (row * WIDTH + x), 0);
            if (++row == HEIGHT) {
                row = 0; phase = 7;
                write32(GC, 0x010200c9); write32(GC + 4, 0x4801);
            }
        } else {
            for (unsigned x = 0; x < FB_WIDTH; x++)
                pixel(2 * ((row + TOP) * WIDTH + x + LEFT), presenter_pixel(x, row));
            if (++row == FB_HEIGHT) row = 0;
        }
        if (time_us() >= until) break;
    }
}
