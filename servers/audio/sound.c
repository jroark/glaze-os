/*
 * System sounds: UI clicks and alert tones, mixed over whatever the output
 * plays through the kernel's mix ring (SYS_AUDIO_MIX).
 *
 * Clips are 22050 Hz mono 16-bit PCM WAVs in romfs "sounds/", made by
 * tools/mksounds.py and used in place. Two voices play at once, a click and
 * an alert; each is mixed at SOUND_RATE, converted to the output's rate
 * and channels, then scaled with music by the kernel's master volume.
 * Samples are kept at most
 * MIX_AHEAD bytes ahead in the mix ring, so a stopped alarm goes quiet
 * within a fraction of a second.
 */

#include "sound.h"
#include <glaze/audio.h>
#include <glaze/media.h>
#include <glaze/prefs.h>
#include <glaze/sys.h>
#include <stdio.h>
#include <string.h>

#define MAX_CLIPS   16
#define MIX_AHEAD   8192                    /* bytes queued in the kernel */
#define CHUNK       256                     /* frames mixed per pass */
#define OUT_SAMPLES (CHUNK * 4 + 16)        /* CHUNK at up to 2x the rate, stereo */
#define GAP_FRAMES  (SOUND_RATE * 3 / 4)    /* between repeats of the timer */
#define LOOP_FRAMES (SOUND_RATE * 60)       /* the timer gives up after this */

struct clip {
    char name[24];
    const int16_t *pcm;                     /* NULL if it is missing or bad */
    uint32_t frames;
};

struct voice {
    const struct clip *clip;                /* NULL = silent */
    uint32_t pos, gap, played;
    bool loop;
};

static struct clip clips[MAX_CLIPS];
static int nclips;
static struct voice click, alert;

/* Settings, read on first use and at AUD_SOUND_RELOAD. */
static bool loaded;
static int32_t gain;                        /* Q15 */
static int32_t sw_touch, sw_keys, sw_swipe, sw_lock;
static int32_t tone_timer, tone_cal, tone_notify;

/* Output conversion. */
static struct media_resampler rs;
static uint32_t out_ch = 1;
static int16_t mixed[CHUNK];
static uint32_t mixed_off, mixed_len;       /* frames */
static int16_t out[OUT_SAMPLES];
static uint32_t out_off, out_len;           /* samples */

static void log_msg(const char *msg)
{
    char line[96];

    snprintf(line, sizeof(line), "audio: %s\n", msg);
    gz_log(line, (uint32_t)strlen(line));
}

/* ---- settings and clips ---------------------------------------------------- */

static int32_t pref_int(const char *key, int32_t def)
{
    int32_t v;

    return pref_get(key, &v, sizeof(v)) == sizeof(v) ? v : def;
}

static int32_t tone_pref(const char *key, int32_t def)
{
    int32_t v = pref_int(key, def);

    return v >= 0 && v < AUDIO_TONES ? v : AUDIO_TONE_NONE;
}

static void load_settings(void)
{
    int32_t vol = pref_int(AUDIO_PREF_VOLUME, AUDIO_VOLUME_DEFAULT);

    vol = vol < 0 ? 0 : vol > AUDIO_VOLUME_MAX ? AUDIO_VOLUME_MAX : vol;
    /* Squared, so the steps sound roughly even. */
    gain = vol * vol * 32768 / (AUDIO_VOLUME_MAX * AUDIO_VOLUME_MAX);
    gz_audio_gain((uint32_t)gain);
    sw_touch = pref_int(AUDIO_PREF_TOUCH, 0);
    sw_keys = pref_int(AUDIO_PREF_KEYS, 1);
    sw_swipe = pref_int(AUDIO_PREF_SWIPE, 1);
    sw_lock = pref_int(AUDIO_PREF_LOCK, 1);
    tone_timer = tone_pref(AUDIO_PREF_TONE_TIMER, AUDIO_TONE_TIMER_DEFAULT);
    tone_cal = tone_pref(AUDIO_PREF_TONE_CAL, AUDIO_TONE_CAL_DEFAULT);
    tone_notify = tone_pref(AUDIO_PREF_TONE_NOTIFY, AUDIO_TONE_NOTIFY_DEFAULT);
    loaded = true;
}

struct mem {
    const uint8_t *data;
    uint32_t size;
};

static int mem_reader(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    const struct mem *m = ctx;

    if (off >= m->size)
        return 0;
    len = MIN(len, m->size - off);
    memcpy(buf, m->data + off, len);
    return (int)len;
}

/* The clip `name`, mapped from romfs once and kept. */
static const struct clip *clip_get(const char *name)
{
    char path[48];
    struct media_wav wav;
    struct mem m;

    if (!name)
        return NULL;
    for (int i = 0; i < nclips; i++)
        if (!strcmp(clips[i].name, name))
            return clips[i].pcm ? &clips[i] : NULL;
    if (nclips == MAX_CLIPS)
        return NULL;
    struct clip *c = &clips[nclips++];
    snprintf(c->name, sizeof(c->name), "%s", name);
    snprintf(path, sizeof(path), "sounds/%s", name);
    m.data = gz_romfs_map(path, &m.size);
    if (!m.data || media_wav_open(mem_reader, &m, m.size, &wav) < 0 || wav.fmt.format != MEDIA_FMT_PCM ||
        wav.fmt.bits != 16 || wav.fmt.channels != 1 || wav.fmt.rate != SOUND_RATE || (wav.data_off & 1)) {
        snprintf(path, sizeof(path), "bad sound %s", name);
        log_msg(path);
        return NULL;
    }
    c->pcm = (const int16_t *)(m.data + wav.data_off);
    c->frames = wav.data_len / 2;
    return c;
}

static void voice_start(struct voice *v, const char *name, bool loop)
{
    const struct clip *c = clip_get(name);

    if (!c)
        return;
    *v = (struct voice){ .clip = c, .loop = loop };
}

/* ---- requests ---------------------------------------------------------------- */

static void play(uint32_t snd)
{
    int32_t tone = AUDIO_TONE_NONE;
    bool loop = false;

    if (!loaded)
        load_settings();
    if (!gain)
        return;
    switch (snd) {
    case AUDIO_SND_TAP:
        if (sw_touch)
            voice_start(&click, "tap.wav", false);
        return;
    case AUDIO_SND_KEY:
        if (sw_keys)
            voice_start(&click, "key.wav", false);
        return;
    case AUDIO_SND_SWIPE:
        if (sw_swipe)
            voice_start(&click, "swipe.wav", false);
        return;
    case AUDIO_SND_LOCK:
    case AUDIO_SND_UNLOCK:
        if (sw_lock)
            voice_start(&click, snd == AUDIO_SND_LOCK ? "lock.wav" : "unlock.wav", false);
        return;
    case AUDIO_SND_NOTIFY:      tone = tone_notify; break;
    case AUDIO_SND_CALENDAR:    tone = tone_cal; break;
    case AUDIO_SND_TIMER:       tone = tone_timer; loop = true; break;
    default:
        if (snd >= AUDIO_SND_TONE && snd < AUDIO_SND_TONE + AUDIO_TONES)
            tone = (int32_t)(snd - AUDIO_SND_TONE);
        break;
    }
    if (tone != AUDIO_TONE_NONE)
        voice_start(&alert, audio_tone_file(tone), loop);
}

void sound_notify(uint32_t bits)
{
    /* Stop and reload first, so a preview after a change uses it. */
    if (bits & AUDIO_NOTIFY_STOP)
        alert.clip = NULL;
    if (bits & AUDIO_NOTIFY_RELOAD)
        load_settings();
    for (uint32_t snd = 0; snd < AUDIO_SND_TONE + AUDIO_TONES; snd++)
        if (bits & 1u << snd)
            play(snd);
}

/* ---- mixing -------------------------------------------------------------------- */

static int32_t voice_next(struct voice *v)
{
    const struct clip *c = v->clip;

    if (!c)
        return 0;
    if (v->loop && ++v->played >= LOOP_FRAMES) {
        v->clip = NULL;
        return 0;
    }
    if (v->pos < c->frames)
        return c->pcm[v->pos++];
    if (!v->loop) {
        v->clip = NULL;
        return 0;
    }
    if (++v->gap >= GAP_FRAMES)
        v->pos = v->gap = 0;
    return 0;
}

static void mix_chunk(void)
{
    for (int i = 0; i < CHUNK; i++) {
        int32_t s = voice_next(&click) + voice_next(&alert);
        mixed[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
    mixed_off = 0;
    mixed_len = CHUNK;
}

void sound_reconfig(uint32_t rate, uint32_t ch)
{
    media_rs_init(&rs, SOUND_RATE, 1, rate, (int)ch);
    out_ch = ch;
    out_off = out_len = 0;
}

bool sound_busy(void)
{
    return click.clip || alert.clip || mixed_len || out_len;
}

bool sound_pump(void)
{
    struct gz_audio_status st;

    for (;;) {
        if (out_len) {
            if (gz_audio_status(&st) < 0 || st.mix_queued >= MIX_AHEAD)
                return true;
            uint32_t frame = 2 * out_ch;
            uint32_t room = (MIX_AHEAD - st.mix_queued) / frame * frame;
            int n = gz_audio_mix(out + out_off, MIN(room, out_len * 2));
            if (n <= 0)
                return true;
            out_off += (uint32_t)n / 2;
            out_len -= (uint32_t)n / 2;
            if (out_len)
                return true;
            out_off = 0;
        }
        if (!mixed_len) {
            if (!click.clip && !alert.clip)
                return false;
            mix_chunk();
        }
        int used;
        int n = media_rs_run(&rs, mixed + mixed_off, (int)mixed_len, out, (int)(OUT_SAMPLES / out_ch), &used);
        mixed_off += (uint32_t)used;
        mixed_len -= (uint32_t)used;
        out_len = (uint32_t)n * out_ch;
    }
}
