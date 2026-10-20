/*
 * Buddy and message sounds: the WAV clips in the bundle's res/ (22050 Hz mono
 * S16, tools/mksounds.py --aim), played through an audio stream at the
 * "Ringer and Alerts" volume. A stream takes the output from the music
 * player, so while music plays an incoming IM uses the system notification
 * sound instead, which mixes over it, and the other sounds are skipped.
 */

#include <glaze/audio.h>
#include <glaze/prefs.h>
#include <glaze/res.h>
#include <glaze/sys.h>
#include <stdlib.h>
#include <string.h>
#include "aim.h"

#define RATE        22050

static const char *const files[SND_COUNT] = { "dooropen.wav", "doorslam.wav", "imrcv.wav", "imsend.wav" };

static struct clip {
    int16_t *pcm;           /* scaled by the volume when loaded */
    uint32_t frames;
    int volume;
} clips[SND_COUNT];

static const struct clip *playing;
static uint32_t pos;
static bool open, ended;
static uint32_t started_ms;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int volume(void)
{
    int32_t v = AUDIO_VOLUME_DEFAULT;

    if (pref_get(AUDIO_PREF_VOLUME, &v, sizeof(v)) != sizeof(v) || v < 0 || v > 10)
        v = AUDIO_VOLUME_DEFAULT;
    return (int)v;
}

/* The data chunk of a 22050 Hz mono 16-bit RIFF WAVE. */
static bool load(struct clip *c, const char *name, int vol)
{
    uint32_t size;
    const uint8_t *p = res_map(name, &size), *data = NULL;
    uint32_t data_len = 0;
    bool fmt_ok = false;

    if (!p || size < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4))
        return false;
    for (uint32_t off = 12; off + 8 <= size;) {
        uint32_t n = le32(p + off + 4);
        if (n > size - off - 8)
            break;
        const uint8_t *body = p + off + 8;
        if (!memcmp(p + off, "fmt ", 4) && n >= 16)
            fmt_ok = body[0] == 1 && body[1] == 0 && body[2] == 1 && le32(body + 4) == RATE && body[14] == 16;
        else if (!memcmp(p + off, "data", 4)) {
            data = body;
            data_len = n;
        }
        off += 8 + n + (n & 1);
    }
    if (!fmt_ok || !data)
        return false;
    free(c->pcm);
    c->frames = data_len / 2;
    c->pcm = malloc(c->frames * 2);
    if (!c->pcm)
        return false;
    for (uint32_t i = 0; i < c->frames; i++) {
        int16_t s = (int16_t)(data[i * 2] | data[i * 2 + 1] << 8);
        c->pcm[i] = (int16_t)(s * vol / 10);
    }
    c->volume = vol;
    return true;
}

static void stop(void)
{
    if (open)
        audio_stream_close();
    open = false;
    playing = NULL;
}

void sound_play(int snd)
{
    struct audio_state st;
    int vol = volume();

    if (snd < 0 || snd >= SND_COUNT || !vol)
        return;
    if (audio_get_state(&st) == 0 && st.state == AUDIO_PLAYING && !open) {
        if (snd == SND_IM_IN)
            audio_sound(AUDIO_SND_NOTIFY);
        return;
    }
    struct clip *c = &clips[snd];
    if ((!c->pcm || c->volume != vol) && !load(c, files[snd], vol))
        return;
    if (!open) {
        if (audio_stream_open(RATE, 1) < 0)
            return;
        open = true;
    }
    /* A newer sound replaces the rest of one still being fed. */
    playing = c;
    pos = 0;
    ended = false;
    started_ms = gz_time_ms();
    sound_pump();
}

bool sound_pump(void)
{
    if (!open)
        return false;
    while (playing && pos < playing->frames) {
        uint32_t left = (playing->frames - pos) * 2;
        int n = audio_stream_write(playing->pcm + pos, left < AUDIO_BUF_SIZE ? left : AUDIO_BUF_SIZE);
        if (n < 0) {
            stop();
            return false;
        }
        if (!n)
            return true;
        pos += (uint32_t)n / 2;
    }
    if (!ended) {
        audio_stream_write(playing->pcm, 0);    /* the end: start even if short */
        ended = true;
    }
    uint32_t played, queued;
    if (audio_stream_clock(&played, &queued) < 0 || !queued || gz_time_ms() - started_ms > 5000) {
        stop();
        return false;
    }
    return true;
}
