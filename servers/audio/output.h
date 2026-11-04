#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H
#include <glaze/abi.h>
/* Select Bluetooth when a speaker is connected as playback is configured.
 * Local notifications retain the device's system-sound output. */
bool output_bluetooth(void);
bool output_lost(void);
void output_config(uint32_t *,uint32_t *);
void output_status(struct gz_audio_status *);
int output_start(uint32_t,uint32_t);
void output_stop(bool);
int output_write(const void *,unsigned);
#endif
