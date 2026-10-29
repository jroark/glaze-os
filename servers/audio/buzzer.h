#ifndef AUDIO_BUZZER_H
#define AUDIO_BUZZER_H
#include <glaze/types.h>
void buzzer_volume(int volume);
void buzzer_click(unsigned sound);
void buzzer_alert(int tone, bool repeat);
void buzzer_stop_alert(void);
/* Advance the pattern and return the next service timeout (-1 if idle). */
int buzzer_service(void);
bool buzzer_busy(void);
#endif
