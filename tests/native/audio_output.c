/* Backend changes preserve local playback and bind Bluetooth producers and
 * clocks to a session; unplug cannot silently continue on the local speaker. */
#include "audio_output_sys.h"
#include "../../servers/audio/output.c"
#include <assert.h>
#include <stdio.h>
static struct bt_audio_status remote;
static int16_t shared_pcm[BT_AUDIO_BUF_SIZE/2];
static int32_t master_gain=32768;
int32_t sound_gain(void){return master_gain;}
static int lookup_error,open_error,write_error,play_error;
static unsigned local_starts,local_stops,local_writes,attaches,detaches,pauses,plays;
int gz_audio_status(struct gz_audio_status *s){memset(s,0,sizeof(*s));s->rate=22050;s->frames_played=123;return 0;}
int gz_audio_start(unsigned r,unsigned c){local_starts++;return 0;}
int gz_audio_stop(bool f){local_stops++;return 0;}
int gz_audio_write(const void *p,unsigned n){local_writes++;return n;}
int bt_audio_status(struct bt_audio_status *s){*s=remote;return lookup_error;}
int bt_audio_stream_open(struct bt_audio_stream *s){attaches++;s->id=remote.id;s->buffer=shared_pcm;return open_error;}
void bt_audio_stream_close(struct bt_audio_stream *s){detaches++;s->buffer=NULL;}
int bt_audio_stream_play(struct bt_audio_stream *s,bool play){if(play)plays++;else pauses++;return play_error;}
int bt_audio_stream_write(struct bt_audio_stream *s,const void *p,unsigned n){return write_error?write_error:(int)n;}
int main(void)
{
    int16_t samples[]={32767,-32768};
    uint32_t rate=22050,ch=1;struct gz_audio_status st;
    output_config(&rate,&ch);assert(!output_bluetooth() && rate==22050 && ch==1);
    assert(!output_start(rate,ch) && local_starts==1);assert(output_write(samples,2)==2 && local_writes==1);output_stop(true);assert(local_stops==1);
    remote=(struct bt_audio_status){.state=BT_AUDIO_STREAMING,.id=7,.rate=44100,.channels=2,.frames_played=1024,.queued=2048};
    output_config(&rate,&ch);assert(output_bluetooth() && rate==44100 && ch==2 && attaches==1);
    output_status(&st);assert(st.frames_played==1024 && st.queued==2048 && !st.running);
    assert(!output_start(rate,ch) && plays==1);output_status(&st);assert(st.running);
    assert(output_write(samples,4)==4 && local_writes==1);
    assert(shared_pcm[0]==32767 && shared_pcm[1]==-32768);
    master_gain=16384;assert(output_write(samples,4)==4 && shared_pcm[0]==16383 && shared_pcm[1]==-16384);
    master_gain=0;assert(output_write(samples,4)==4 && !shared_pcm[0] && !shared_pcm[1]);master_gain=32768;
    write_error=GZ_EBUSY;assert(output_write(samples,4)==GZ_EBUSY && !output_lost());write_error=0;
    output_stop(false);assert(pauses==1 && output_bluetooth() && !detaches);
    assert(!output_start(rate,ch) && plays==2);
    remote.id=8;remote.frames_played=0;output_status(&st);
    assert(output_lost() && st.frames_played==1024);assert(output_write(samples,4)==GZ_ENOTCONN);
    output_stop(true);assert(detaches==1 && !output_bluetooth() && !output_lost() && local_starts==1);
    remote.rate=123;rate=22050;ch=1;output_config(&rate,&ch);assert(!output_bluetooth() && rate==22050);
    remote.rate=48000;open_error=GZ_EBUSY;output_config(&rate,&ch);assert(!output_bluetooth() && rate==22050);
    open_error=0;output_config(&rate,&ch);assert(output_bluetooth() && rate==48000);lookup_error=GZ_EDEAD;
    output_status(&st);assert(output_lost());output_stop(true);
    puts("Audio output: local routing, negotiated format, producer backpressure, pause/resume, stale session, card loss and clock preservation PASS");
    return 0;
}
