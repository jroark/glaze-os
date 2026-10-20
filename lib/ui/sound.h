/* libui internals: touch sounds (sound.c). Not exported. */
#ifndef LIBUI_SOUND_H
#define LIBUI_SOUND_H

#include <glaze/abi.h>

/* Sees each event ui_next_event returns: a tap or swipe makes its sound. */
void uisnd_event(const struct gz_input_event *e);
/* The current touch made its own sound (a keyboard key): no tap tick. */
void uisnd_sounded(void);

#endif
