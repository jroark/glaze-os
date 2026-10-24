#include "nes.h"
#include <string.h>

uint32_t nes_hash(const void *data, size_t size)
{
    const uint8_t *p = data;
    uint32_t h = 2166136261u;
    while (size--) h = (h ^ *p++) * 16777619u;
    return h;
}

/* NES 2.0 uses either 12-bit bank counts or a byte-size exponent/multiplier.
 * Bound the exponent before shifting so malformed headers cannot wrap sizes.
 * Format: https://www.nesdev.org/wiki/NES_2.0
 */
static bool rom_size(uint8_t low, unsigned high, uint32_t unit, uint32_t *size)
{
    if (high == 15) {
        unsigned exponent = low >> 2, multiplier = (low & 3) * 2 + 1;
        if (exponent >= 32 || (1u << exponent) > NES_ROM_MAX / multiplier)
            return false;
        *size = (1u << exponent) * multiplier;
    } else {
        *size = ((high << 8) | low) * unit;
    }
    return *size <= NES_ROM_MAX;
}

static uint32_t ram_size(unsigned shift)
{
    return shift ? 64u << shift : 0;
}

const char *nes_validate(const uint8_t *rom, size_t size, struct nes_cart *cart)
{
    if (size < 16 || memcmp(rom, "NES\032", 4)) return "Not an iNES ROM";
    struct nes_cart c = {0};
    c.nes2 = (rom[7] & 0x0c) == 0x08;
    c.mapper = (rom[6] >> 4) | (rom[7] & 0xf0);
    c.trainer = (rom[6] & 4) != 0;
    c.battery = (rom[6] & 2) != 0;
    if (c.nes2) {
        if (rom[7] & 3) return "Only standard NES consoles are supported";
        c.mapper |= (rom[8] & 15) << 8;
        c.submapper = rom[8] >> 4;
        if (c.mapper > 4) return "Supported mappers: 0, 1, 2, 3 and 4";
        if (c.submapper) return "NES 2.0 submapper is not supported";
        if ((rom[12] & 0xfc) || rom[13] || (rom[14] & 0xfc) || (rom[15] & 0xc0))
            return "Invalid NES 2.0 reserved fields";
        /* Multi-region cartridges run in NTSC mode; PAL-only/Dendy cannot. */
        if ((rom[12] & 3) == 1 || (rom[12] & 3) == 3)
            return "PAL and Dendy timing are not supported";
        if (rom[14] & 3) return "Miscellaneous ROM data is not supported";
        if ((rom[15] & 0x3f) > 1) return "Expansion device is not supported";
        if (!rom_size(rom[4], rom[9] & 15, 16384, &c.prg_size) ||
            !rom_size(rom[5], rom[9] >> 4, 8192, &c.chr_size))
            return "Cartridge ROM size exceeds the memory limit";
        c.prg_ram_size = ram_size(rom[10] & 15);
        c.prg_nvram_size = ram_size(rom[10] >> 4);
        c.chr_ram_size = ram_size(rom[11] & 15);
        uint32_t chr_nvram_size = ram_size(rom[11] >> 4);
        if ((c.prg_nvram_size || chr_nvram_size) != c.battery)
            return "Battery flag does not match nonvolatile RAM";
        if (c.prg_ram_size && c.prg_nvram_size)
            return "Mixed volatile and battery PRG RAM is not supported";
        if ((c.prg_ram_size && c.prg_ram_size != NES_SAVE_SIZE) ||
            (c.prg_nvram_size && c.prg_nvram_size != NES_SAVE_SIZE))
            return "Only 8 KiB PRG RAM banks are supported";
        if (chr_nvram_size) return "Battery CHR RAM is not supported";
        if (c.chr_size ? c.chr_ram_size != 0 : c.chr_ram_size != 8192)
            return "Cartridge requires an unsupported CHR RAM layout";
        if (c.trainer && !c.prg_ram_size && !c.prg_nvram_size)
            return "Trainer requires 8 KiB PRG RAM";
    } else {
        if (rom[7] & 0x0f) return "Only standard iNES cartridges are supported";
        if (rom[9] & 1) return "PAL ROMs are not supported";
        /* Reject garbage-header dumps rather than guessing mapper bits. */
        for (int i = 10; i < 16; i++)
            if (rom[i]) return "Invalid iNES reserved bytes";
        if (rom[8] > 1) return "Cartridge needs more than 8 KiB save RAM";
        c.prg_size = rom[4] * 16384u;
        c.chr_size = rom[5] * 8192u;
        c.prg_ram_size = c.battery ? 0 : NES_SAVE_SIZE;
        c.prg_nvram_size = c.battery ? NES_SAVE_SIZE : 0;
        c.chr_ram_size = c.chr_size ? 0 : 8192;
    }
    if (c.mapper > 4) return "Supported mappers: 0, 1, 2, 3 and 4";
    if ((c.prg_size % 16384) || (c.chr_size % 8192))
        return "ROM sizes must align to supported cartridge banks";
    unsigned prg = c.prg_size / 16384, chr = c.chr_size / 8192;
    if (!prg || prg > 32 || chr > 32 || (prg & (prg-1)) || (chr && (chr & (chr-1))))
        return "Unsupported cartridge bank sizes";
    unsigned m = c.mapper;
    if ((m == 0 || m == 3) && (prg > 2 || (m == 0 && chr > 1) || (m == 3 && !chr)))
        return "Invalid NROM/CNROM bank sizes";
    if ((m == 1 && chr > 16) || (m == 2 && (prg < 2 || chr)) || (m == 4 && prg < 2))
        return "Unsupported mapper bank configuration";
    if ((rom[6] & 8) && m != 4) return "Four-screen mirroring requires mapper 4";
    c.prg_offset = 16 + (c.trainer ? 512 : 0);
    c.chr_offset = c.prg_offset + c.prg_size;
    if (size > NES_ROM_MAX || size != c.chr_offset + c.chr_size)
        return "Truncated ROM or unexpected trailing data";
    c.hash = nes_hash(rom, size);
    *cart = c;
    return NULL;
}

uint16_t nes_rgb565(uint16_t c)
{
    /* Bit 15 is InfoNES's background-priority flag, not a colour bit. */
    return ((c & 0x7fe0) << 1) | (c & 31) | ((c >> 4) & 32);
}
