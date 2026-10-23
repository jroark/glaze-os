/* Portable cartridge validation, RGB conversion and controller state. */
#ifndef GLAZE_NES_H
#define GLAZE_NES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define NES_ROM_MAX (1024u * 1024u)
#define NES_SAVE_SIZE 8192u
#define NES_A 1u
#define NES_B 2u
#define NES_SELECT 4u
#define NES_START 8u
#define NES_UP 16u
#define NES_DOWN 32u
#define NES_LEFT 64u
#define NES_RIGHT 128u
struct nes_cart {
    uint32_t prg_size, chr_size, prg_offset, chr_offset, hash;
    unsigned mapper;
    bool battery, trainer;
};
const char *nes_validate(const uint8_t *rom, size_t size, struct nes_cart *cart);
uint32_t nes_hash(const void *data, size_t size);
uint16_t nes_rgb565(uint16_t rgb555);
#endif
