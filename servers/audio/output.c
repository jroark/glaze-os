/* Output selection is private to the audio service. Public player and stream
 * APIs keep their existing PCM format, resampling and position semantics. */
#include "output.h"
#include "sound.h"
#include <glaze/bluetooth.h>
#include <glaze/sys.h>
#include <string.h>
static struct bt_audio_stream stream;
static struct bt_audio_status last;
static bool bluetooth,lost,running;
bool output_bluetooth(void){return bluetooth;}
bool output_lost(void){return lost;}
void output_config(uint32_t *rate,uint32_t *channels)
{
    if(bluetooth)bt_audio_stream_close(&stream);
    bluetooth=lost=running=false;
    struct bt_audio_status st;
    if(bt_audio_status(&st) || st.state!=BT_AUDIO_STREAMING ||
       (st.rate!=44100 && st.rate!=48000) || (st.channels!=1 && st.channels!=2))return;
    if(bt_audio_stream_open(&stream))return;
    bluetooth=true;last=st;*rate=st.rate;*channels=st.channels;
}
void output_status(struct gz_audio_status *st)
{
    if(!bluetooth){if(gz_audio_status(st)<0)memset(st,0,sizeof(*st));return;}
    struct bt_audio_status next;
    int error=bt_audio_status(&next);
    if(error || next.id!=stream.id || next.state!=BT_AUDIO_STREAMING)lost=true;
    else last=next;
    memset(st,0,sizeof(*st));st->rate=last.rate;st->channels=last.channels;st->running=running;
    st->queued=last.queued;st->frames_played=last.frames_played;st->underruns=last.underruns;
}
int output_start(uint32_t rate,uint32_t channels)
{
    if(!bluetooth)return gz_audio_start(rate,channels);
    int error=bt_audio_stream_play(&stream,true);if(!error)running=true;else lost=true;return error;
}
void output_stop(bool flush)
{
    if(!bluetooth){gz_audio_stop(flush);return;}
    running=false;
    if(flush){bt_audio_stream_close(&stream);gz_audio_stop(true);bluetooth=lost=false;}
    else if(bt_audio_stream_play(&stream,false))lost=true;
}
int output_write(const void *p,unsigned n)
{
    if(!bluetooth)return gz_audio_write(p,n);
    if(lost)return GZ_ENOTCONN;
    n=MIN(n,BT_AUDIO_BUF_SIZE);
    int16_t *scaled=stream.buffer;const int16_t *samples=p;int32_t gain=sound_gain();
    for(unsigned i=0;i<n/2;i++)scaled[i]=(int16_t)(((int32_t)samples[i]*gain)>>15);
    int result=bt_audio_stream_write(&stream,scaled,n);
    if(result<0 && result!=GZ_EBUSY)lost=true;
    return result;
}
