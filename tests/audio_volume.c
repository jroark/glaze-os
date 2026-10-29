/* Settings reload must update master gain without pre-scaling effects. */
#include <glaze/audio.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define GLAZE_SYS_H
static int32_t saved_volume = AUDIO_VOLUME_DEFAULT;
static uint32_t kernel_gain, gain_calls;
static uint32_t now_ms,buzz_hz,buzz_ms,buzz_pulse,buzz_calls;
uint32_t gz_time_ms(void) { return now_ms; }
int gz_buzzer_tone(uint32_t hz,uint32_t ms,uint32_t pulse)
{ buzz_hz=hz;buzz_ms=ms;buzz_pulse=pulse;buzz_calls++;return 0; }
int pref_get(const char *key, void *out, uint32_t size)
{
    if (strcmp(key, AUDIO_PREF_VOLUME))
        return GZ_ENOENT;
    assert(size == sizeof(saved_volume));
    memcpy(out, &saved_volume, size);
    return (int)size;
}
int gz_audio_gain(uint32_t gain)
{
    kernel_gain = gain;
    gain_calls++;
    return 0;
}
int gz_log(const char *s, uint32_t n) { (void)s; (void)n; return 0; }
const void *gz_romfs_map(const char *name, uint32_t *n)
{
    (void)name; (void)n;
    return NULL;
}
int gz_audio_status(struct gz_audio_status *st) { memset(st, 0, sizeof(*st)); return 0; }
int gz_audio_mix(const void *pcm, uint32_t size) { (void)pcm; return (int)size; }

#include "../servers/audio/sound.c"

int main(void)
{
    sound_notify(AUDIO_NOTIFY_RELOAD);
    assert(kernel_gain == 7 * 7 * 32768 / 100 && gain_calls == 1);
    assert(!buzz_calls); /* no startup tone */
    saved_volume = 5;
    sound_notify(AUDIO_NOTIFY_RELOAD);
    assert(kernel_gain == 8192 && gain_calls == 2);
    const int16_t a[] = {12000, -12000}, b[] = {4000, -4000};
    struct clip ca = {.pcm = a, .frames = 2}, cb = {.pcm = b, .frames = 2};
    click = (struct voice){.clip = &ca};
    alert = (struct voice){.clip = &cb};
    mix_chunk();
    assert(mixed[0] == 16000 && mixed[1] == -16000); /* master scales once, later */
    saved_volume = 0;
    sound_notify(AUDIO_NOTIFY_RELOAD);
    assert(kernel_gain == 0);
    saved_volume = AUDIO_VOLUME_MAX;
    sound_notify(AUDIO_NOTIFY_RELOAD);
    assert(kernel_gain == 32768);
    assert(!buzz_hz);
    sound_notify(1u<<AUDIO_SND_KEY);
    assert(buzz_hz==2200 && buzz_ms==12 && buzz_pulse==50);
    now_ms=12;assert(buzzer_service()==-1 && !buzz_hz);
    sound_notify(1u<<AUDIO_SND_TIMER);
    assert(buzz_hz==1800 && buzz_pulse==50);
    sound_notify(1u<<AUDIO_SND_KEY);assert(buzz_hz==1800); /* alert wins */
    now_ms+=160;assert(buzzer_service()==120 && !buzz_hz);
    now_ms+=120;assert(buzzer_service()==160 && buzz_hz==1800);
    sound_notify(AUDIO_NOTIFY_STOP);assert(!buzz_hz && buzzer_service()==-1);
    sound_notify(1u<<(AUDIO_SND_TONE+0));assert(buzz_hz==880);
    saved_volume=3;sound_notify(AUDIO_NOTIFY_RELOAD);assert(buzz_hz==880 && buzz_pulse==15);
    saved_volume=0;sound_notify(AUDIO_NOTIFY_RELOAD);assert(!buzz_hz && buzzer_service()==-1);
    sound_notify(1u<<AUDIO_SND_KEY);assert(!buzz_hz);
    saved_volume = -1;
    sound_notify(AUDIO_NOTIFY_RELOAD);
    assert(kernel_gain == 0);
    saved_volume = AUDIO_VOLUME_MAX + 1;
    sound_notify(AUDIO_NOTIFY_RELOAD);
    assert(kernel_gain == 32768);
    puts("audio volume: PASS preference reload, mute/full/clamping and single gain stage");
    return 0;
}
