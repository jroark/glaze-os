/* Optional real-kernel integration fixture: RADIO_TESTS=1 INIT=bin/radio_guest. */
#include <glaze/audio.h>
#include <glaze/net.h>
#include <glaze/radio.h>
#include <glaze/sys.h>
#include <stdio.h>
#include <string.h>
#ifndef RADIO_TEST_URL
#define RADIO_TEST_URL "https://somafm.com/groovesalad.pls"
#endif
static int port, session;
static void *buffer;
static void check(bool ok, const char *what)
{
    if (ok) return;
    char text[160]; snprintf(text,sizeof(text),"radio guest: FAIL %s\n",what);
    gz_log(text,(uint32_t)strlen(text)); gz_exit(1);
}
static int call(int op)
{
    struct gz_msg m={{0}},r={{0}}; m.w[0]=(uint32_t)op; m.w[1]=(uint32_t)session;
    check(gz_send(port,&m,&r)>=0,"IPC"); return (int)r.w[0];
}
static struct radio_state snapshot(void)
{
    check(!call(AUD_RADIO_STATE),"radio state");
    return *(struct radio_state *)buffer;
}
static void play(void)
{
    struct radio_request req={0};
    strcpy(req.id,"fixture"); strcpy(req.name,"Radio integration fixture");
    snprintf(req.playlist,sizeof(req.playlist),"%s",RADIO_TEST_URL);
    memcpy(buffer,&req,sizeof(req)); check(!call(AUD_RADIO_PLAY),"play radio");
    uint32_t start=gz_time_ms(); struct radio_state state;
    do {
        state=snapshot();
        if (state.phase==RADIO_ERROR) {
            char text[96]; snprintf(text,sizeof(text),"radio guest: phase=%u error=%u\n",state.phase,state.error);
            gz_log(text,(uint32_t)strlen(text));
        }
        check(state.phase!=RADIO_ERROR,"radio entered error state");
        check(state.buffered_bytes<=128*1024,"bounded buffer");
        check(gz_time_ms()-start<30000,"playback startup timeout");
        if (state.phase==RADIO_PLAYING) break;
        gz_sleep_ms(25);
    } while (true);
}
static void stopped(void)
{
    check(snapshot().phase==RADIO_STOPPED,"radio stopped on takeover");
    gz_sleep_ms(500);
    check(snapshot().phase==RADIO_STOPPED,"no stale reconnect after takeover");
}
int main(void)
{
    check(!net_wait_up(15000),"network startup");
    for (int i=0;(port=gz_port_lookup(AUDIO_PORT_NAME))<=0;i++) {
        check(i<200,"audio service startup"); gz_sleep_ms(25);
    }
    struct gz_msg m={{0}},r={{0}};
    m.w[0]=AUD_HELLO; m.w[1]=(uint32_t)gz_getpid();
    check(gz_send(port,&m,&r)>=0 && !(int)r.w[0],"audio session");
    session=(int)r.w[1]; buffer=gz_shm_map((int)r.w[2]); check(buffer!=NULL,"shared buffer");
    check(!call(AUD_RADIO_STATE),"radio capability");
    play();
    struct audio_state before,after;
    check(!audio_get_state(&before),"audio state before sound");
    check(!audio_sound(AUDIO_SND_TAP),"system sound during radio");
    gz_sleep_ms(500);
    check(snapshot().phase==RADIO_PLAYING,"radio survives system sound");
    check(!audio_get_state(&after) && after.streaming,"radio owns stream output");
    /* Reopening AUD_HELLO in this process must keep radio alive. */
    check(!audio_play("/Music/Demo.mp3"),"local MP3 takeover");
    stopped();
    check(!audio_get_state(&after) && after.state==AUDIO_PLAYING && !after.streaming,"local player state");
    check(!audio_pause() && !audio_resume() && !audio_seek(1000),"local controls");
    play();
    check(!audio_stream_open(22050,1),"PCM stream takeover");
    stopped();
    int16_t pcm[2048]; memset(pcm,0,sizeof(pcm));
    check(audio_stream_write(pcm,sizeof(pcm))>0,"PCM stream accepts data");
    check(!audio_stream_close(),"PCM stream closes");
    play();
    check(audio_seek(1000)==GZ_EINVAL,"live radio is not seekable");
    check(!call(AUD_RADIO_STOP),"explicit radio stop"); stopped();
    struct gz_sysinfo si; gz_sysinfo(&si);
    char text[160];
    snprintf(text,sizeof(text),"radio guest: PASS radio, system sounds, MP3/PCM takeovers, stop; free=%u KiB underruns=%u->%u\n",
        si.free_pages*4,before.underruns,after.underruns);
    gz_log(text,(uint32_t)strlen(text));
    return 0;
}
