/* Original Glaze integer delta accumulator; not upstream Blip_Buffer. */
#ifndef GLAZE_PCE_AUDIO_H
#define GLAZE_PCE_AUDIO_H
#include <stdint.h>
typedef struct { int32_t delta[1024], current; unsigned phase,count; int16_t pcm[1024]; bool overflow; } Blip_Buffer;
typedef struct { int unused; } Blip_Synth;
void Blip_Synth_offset(Blip_Synth *s,int timestamp,int delta,Blip_Buffer *buffer);
void glaze_pce_audio_end(Blip_Buffer *b,unsigned clocks);
#endif
