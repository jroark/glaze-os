/* System sounds of the audio server (sound.c), used by main.c. */
#ifndef AUDIO_SOUND_H
#define AUDIO_SOUND_H

#include <glaze/types.h>

/* The rate the clips are made at, which the output uses when it is started
 * for sounds alone. */
#define SOUND_RATE  22050

/* AUDIO_NOTIFY_* bits from the port. */
void sound_notify(uint32_t bits);
/* The output was set up for `rate`/`ch` and its rings flushed. */
void sound_reconfig(uint32_t rate, uint32_t ch);
/* A voice is playing or converted samples are waiting. */
bool sound_busy(void);
/* Tops up the kernel's mix ring while the output runs; true while there is
 * more to come. */
bool sound_pump(void);

#endif
