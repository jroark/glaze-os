/* Integer step reconstruction for timestamped HuC6280 PSG deltas.
 * This replaces Blip's floating-point setup and bandlimited filter. */
#include <stdbool.h>
#include <string.h>
#include "../../third_party/beetle-pce-fast/include/blip/Blip_Buffer.h"
#define PCE_CLOCK 7159091u
void Blip_Synth_offset(Blip_Synth *s,int timestamp,int delta,Blip_Buffer *b) {
    (void)s;
    if(timestamp<0 || timestamp>180000) { b->overflow=true;return; }
    unsigned at=((unsigned)timestamp*22050u+b->phase)/PCE_CLOCK;
    if(at>=1024) { b->overflow=true;return; }
    b->delta[at]+=delta;
}
void glaze_pce_audio_end(Blip_Buffer *b,unsigned clocks) {
    if(clocks>180000) { b->overflow=true;return; }
    unsigned time=clocks*22050u+b->phase;
    b->count=time/PCE_CLOCK;b->phase=time%PCE_CLOCK;
    if(b->count>=1024) { b->overflow=true;return; }
    for(unsigned i=0;i<b->count;i++) {
        b->current+=b->delta[i];int sample=b->current*2/3;
        if(sample>32767)sample=32767;
        if(sample< -32768)sample= -32768;
        b->pcm[i]=sample;
    }
    unsigned remain=1024-b->count;
    memmove(b->delta,b->delta+b->count,remain*sizeof b->delta[0]);
    memset(b->delta+remain,0,b->count*sizeof b->delta[0]);
}
