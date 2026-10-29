/* SPDX-License-Identifier: GPL-3.0-or-later
 * Eight DMX PCM voices, fixed-point nearest-neighbour resampling to mono. */
#include "port.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include <glaze/audio.h>
#include "doomgeneric.h"
#define VOICES 8
#define RATE 11025
static struct voice { const uint8_t *data; uint32_t length,phase,step; int lump,volume; } voices[VOICES];
static int enabled, opened;
static uint32_t last_ms;
int snd_sfxdevice=SNDDEVICE_SB, snd_musicdevice=SNDDEVICE_NONE;
int snd_samplerate=RATE, snd_cachesize=0, snd_maxslicetime_ms=28;
char *snd_musiccmd="";
void I_StopSound(int ch)
{
    if (ch<0 || ch>=VOICES || !voices[ch].data) return;
    int lump=voices[ch].lump; voices[ch].data=NULL;
    for (int i=0;i<VOICES;i++) if (voices[i].data && voices[i].lump==lump) return;
    W_ReleaseLumpNum(lump);
}
void doom_audio_suspend(void)
{
    for (int i=0;i<VOICES;i++) I_StopSound(i);
    if (opened) audio_stream_close();
    opened=0;
}
void doom_audio_resume(void)
{
    last_ms=DG_GetTicksMs();
    if (enabled) opened=audio_stream_open(RATE,1)>=0;
}
void I_InitSound(boolean prefix)
{
    (void)prefix; enabled=1; doom_audio_resume();
    printf("doom: sound effects %s\n",opened ? "ready" : "unavailable (continuing silently)");
}
void I_ShutdownSound(void) { doom_audio_suspend(); enabled=0; }
int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char name[12]; snprintf(name,sizeof name,"ds%s",sfx->name); return W_GetNumForName(name);
}
void I_UpdateSoundParams(int ch,int volume,int sep)
{
    (void)sep;
    if (ch>=0 && ch<VOICES) voices[ch].volume=volume<0 ? 0 : volume>127 ? 127 : volume;
}
int I_StartSound(sfxinfo_t *sfx,int ch,int vol,int sep)
{
    if (!opened || ch<0 || ch>=VOICES) return -1;
    I_StopSound(ch);
    int lump=sfx->lumpnum, len=W_LumpLength(lump);
    if (len<40) return -1;
    const uint8_t *p=W_CacheLumpNum(lump,PU_STATIC);
    unsigned rate=p[2]|p[3]<<8;
    uint32_t n=(uint32_t)p[4]|(uint32_t)p[5]<<8|(uint32_t)p[6]<<16|(uint32_t)p[7]<<24;
    if (p[0]!=3 || p[1] || rate<4000 || rate>48000 || n<32 || n>(unsigned)len-8 || n>65535) {
        /* Another channel may still own this lump. */
        int busy=0; for (int i=0;i<VOICES;i++) if (voices[i].data && voices[i].lump==lump) busy=1;
        if (!busy) W_ReleaseLumpNum(lump);
        return -1;
    }
    voices[ch]=(struct voice){p+24,n-32,0,(rate<<16)/RATE,lump,0};
    I_UpdateSoundParams(ch,vol,sep); return ch;
}
boolean I_SoundIsPlaying(int ch) { return ch>=0 && ch<VOICES && voices[ch].data!=NULL; }
void I_UpdateSound(void)
{
    uint32_t now=DG_GetTicksMs(), ms=now-last_ms;
    if (!opened || ms<10) return;
    last_ms=now;
    if (ms>50) ms=50;
    int16_t pcm[552]; unsigned frames=ms*RATE/1000;
    for (unsigned n=0;n<frames;n++) {
        int sum=0;
        for (int i=0;i<VOICES;i++) if (voices[i].data) {
            struct voice *v=&voices[i]; unsigned at=v->phase>>16;
            if (at>=v->length) { I_StopSound(i); continue; }
            sum+=((int)v->data[at]-128)*v->volume;
            v->phase+=v->step;
        }
        sum/=2;
        pcm[n]=sum<-32768 ? -32768 : sum>32767 ? 32767 : sum;
    }
    uint32_t played,queued;
    if (audio_stream_clock(&played,&queued)>=0 && queued>100) return;
    int rv=audio_stream_write(pcm,frames*2);
    if (rv<0) { doom_audio_suspend(); return; }
    if (rv>0) audio_stream_write(NULL,0);
}
void I_PrecacheSounds(sfxinfo_t *sounds,int count) { (void)sounds; (void)count; }
void I_BindSoundVariables(void) {}
void I_InitMusic(void) {}
void I_ShutdownMusic(void) {}
void I_SetMusicVolume(int v) { (void)v; }
void I_PauseSong(void) {}
void I_ResumeSong(void) {}
void *I_RegisterSong(void *p,int len) { (void)p; (void)len; return NULL; }
void I_UnRegisterSong(void *h) { (void)h; }
void I_PlaySong(void *h,boolean loop) { (void)h; (void)loop; }
void I_StopSong(void) {}
boolean I_MusicIsPlaying(void) { return false; }
