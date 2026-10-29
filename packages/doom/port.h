/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GLAZE_DOOM_PORT_H
#define GLAZE_DOOM_PORT_H
#include <stdint.h>
void doom_pump(void);
void doom_present(const uint8_t *, const uint16_t *);
void doom_failure(const char *) __attribute__((noreturn));
void doom_audio_suspend(void);
void doom_audio_resume(void);
int doom_map_key(unsigned, int);
void doom_input_pulse(unsigned);
void doom_input_text(unsigned);
void doom_input_release(void);
void doom_input_key(unsigned, int, int);
void doom_input_run(int);
int doom_input_next(int *, unsigned char *);
const char *doom_check_wad(const char *, uint32_t *);
#endif
