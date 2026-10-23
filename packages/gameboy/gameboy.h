/* Portable Game Boy cartridge/core adapter. ROM storage is borrowed until close. */
#ifndef GLAZE_GAMEBOY_H
#define GLAZE_GAMEBOY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define GB_ROM_MAX (8u*1024u*1024u)
#define GB_RIGHT 1u
#define GB_LEFT 2u
#define GB_UP 4u
#define GB_DOWN 8u
#define GB_A 16u
#define GB_B 32u
#define GB_SELECT 64u
#define GB_START 128u
#define GB_RTC_SIZE 32u
struct gb_cart {
    uint32_t rom_size, ram_size, hash;
    unsigned mapper;
    bool cgb, battery, clock, checksum_warning;
    char title[17];
};
extern struct gb_cart gb_cartridge;
const char *gb_validate(const uint8_t *data,size_t size,struct gb_cart *cart);
uint32_t gb_hash(const void *data,size_t size);
const char *gb_open(const uint8_t *data,size_t size,uint32_t wall);
void gb_close(void);
void gb_reset(void);
bool gb_frame(unsigned keys,bool render,uint32_t wall);
const uint16_t *gb_pixels(void);
uint8_t *gb_ram(void);
void gb_clock_sync(uint32_t wall);
void gb_clock_export(uint8_t out[GB_RTC_SIZE],uint32_t wall);
bool gb_clock_import(const uint8_t in[GB_RTC_SIZE],uint32_t wall);
/* Platform callbacks, also supplied by native tests. */
void gb_audio(const int16_t *pcm,unsigned count);
void gb_serial(uint8_t value);
void gb_log(const char *message);
#endif
