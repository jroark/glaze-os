#include "nes.h"
#include <string.h>

uint32_t nes_hash(const void *data, size_t size)
{
    const uint8_t *p = data;
    uint32_t h = 2166136261u;
    while (size--) h = (h ^ *p++) * 16777619u;
    return h;
}

const char *nes_validate(const uint8_t *rom, size_t size, struct nes_cart *c)
{
    if (size < 16 || memcmp(rom, "NES\032", 4)) return "Not an iNES ROM";
    if ((rom[7] & 0x0c) == 0x08) return "NES 2.0 ROMs are not supported";
    if (rom[7] & 0x0f) return "Only standard NES cartridges are supported";
    if (rom[9] & 1) return "PAL ROMs are not supported";
    /* Reject archaic garbage-header dumps rather than guessing mapper bits. */
    for (int i = 10; i < 16; i++)
        if (rom[i]) return "Invalid iNES reserved bytes";
    if (rom[8] > 1) return "Cartridge needs more than 8 KiB save RAM";
    unsigned m = (rom[6] >> 4) | (rom[7] & 0xf0);
    if (m > 4) return "Supported mappers: 0, 1, 2, 3 and 4";
    unsigned prg = rom[4], chr = rom[5];
    if (!prg || prg > 32 || chr > 32 || (prg & (prg-1)) || (chr && (chr & (chr-1))))
        return "Unsupported cartridge bank sizes";
    if ((m == 0 || m == 3) && (prg > 2 || (m == 0 && chr > 1) || (m == 3 && !chr)))
        return "Invalid NROM/CNROM bank sizes";
    if ((m == 1 && chr > 16) || (m == 2 && (prg < 2 || chr)) || (m == 4 && prg < 2))
        return "Unsupported mapper bank configuration";
    if ((rom[6] & 8) && m != 4) return "Four-screen mirroring requires mapper 4";
    c->mapper = m;
    c->prg_size = prg * 16384u;
    c->chr_size = chr * 8192u;
    c->trainer = (rom[6] & 4) != 0;
    c->battery = (rom[6] & 2) != 0;
    c->prg_offset = 16 + (c->trainer ? 512 : 0);
    c->chr_offset = c->prg_offset + c->prg_size;
    if (size > NES_ROM_MAX || size != c->chr_offset + c->chr_size)
        return "Truncated ROM or unexpected trailing data";
    c->hash = nes_hash(rom, size);
    return NULL;
}

uint16_t nes_rgb565(uint16_t c)
{
    /* Bit 15 is InfoNES's background-priority flag, not a colour bit. */
    return ((c & 0x7fe0) << 1) | (c & 31) | ((c >> 4) & 32);
}
