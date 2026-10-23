#ifndef GLAZE_NES_CORE_H
#define GLAZE_NES_CORE_H
#include "nes.h"
extern struct nes_cart nes_cartridge;
const char *nes_open(const uint8_t *data, size_t size);
void nes_close(void);
void nes_run(void);
void nes_reset(void);
uint8_t *nes_sram(void);
/* Platform callbacks; input returns bit 8 to leave emulation. */
void nes_video(const uint16_t *rgb555);
void nes_audio(const int16_t *pcm, unsigned samples);
void nes_audio_open(unsigned rate);
void nes_audio_close(void);
unsigned nes_poll(void);
void nes_log(const char *message);
#endif
