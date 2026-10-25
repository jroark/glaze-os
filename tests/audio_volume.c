/* Settings reload must update master gain without pre-scaling effects. */
#include <glaze/audio.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define GLAZE_SYS_H
static int32_t saved_volume = AUDIO_VOLUME_DEFAULT;
static uint32_t kernel_gain, gain_calls;
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
    saved_volume = -1;
    sound_notify(AUDIO_NOTIFY_RELOAD);
    assert(kernel_gain == 0);
    saved_volume = AUDIO_VOLUME_MAX + 1;
    sound_notify(AUDIO_NOTIFY_RELOAD);
    assert(kernel_gain == 32768);
    puts("audio volume: PASS preference reload, mute/full/clamping and single gain stage");
    return 0;
}
