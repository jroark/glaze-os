#ifndef AUDIO_VOLUME_SYS_H
#define AUDIO_VOLUME_SYS_H
#define GLAZE_SYS_H
#include <glaze/abi.h>
uint32_t gz_time_ms(void);
int gz_buzzer_tone(uint32_t hz,uint32_t ms,uint32_t pulse);
#endif
