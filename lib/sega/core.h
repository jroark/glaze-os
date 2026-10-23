/* Portable adapter shared by the two independently linked Sega packages. */
#ifndef GLAZE_SEGA_CORE_H
#define GLAZE_SEGA_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SEGA_ROM_MAX (4u*1024u*1024u)
#define SEGA_UP 1u
#define SEGA_DOWN 2u
#define SEGA_LEFT 4u
#define SEGA_RIGHT 8u
#define SEGA_A 16u
#define SEGA_B 32u
#define SEGA_C 64u
#define SEGA_START 128u
struct sega_cart { uint32_t hash,rom_size,ram_size; bool pal,battery; char title[49]; };
extern struct sega_cart sega_cartridge;
const char *sega_validate(const uint8_t *rom,size_t size,struct sega_cart *cart);
const char *sega_open(const uint8_t *rom,size_t size);
void sega_close(void);
void sega_reset(void);
bool sega_frame(unsigned buttons,bool render);
const uint16_t *sega_pixels(void);
unsigned sega_width(void);
unsigned sega_height(void);
uint8_t *sega_ram(void);
void sega_set_pal(bool pal);
uint32_t sega_hash(const void *data,size_t size);
void sega_audio(const int16_t *pcm,unsigned count);
void sega_log(const char *message);
#endif
