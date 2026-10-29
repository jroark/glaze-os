/*
 * Audio: client API and the protocol of the audio server (servers/audio),
 * the one process that drives the wave output (SYS_AUDIO_*).
 *
 * The server plays in one of two modes at a time:
 *  - Player: it decodes a file itself (MP3, or WAV as PCM or IMA ADPCM), so
 *    music keeps playing after the app that started it exits. When a track
 *    ends it moves on to the next one in the same folder.
 *  - Stream: one client writes PCM (S16LE, any rate, mono or stereo), which
 *    the server converts to a hardware rate. Opening a stream pauses the
 *    player. The Video app uses it, with the stream clock as its timebase.
 *
 * Sessions work like the file server's: AUD_HELLO returns a session and a
 * shared-memory buffer for paths, state and PCM.
 *
 * System sounds need no session: they are asked for with notification
 * bits on the server's port (AUDIO_NOTIFY_*), so the compositor and apps
 * never wait for the server. They play over either mode through the
 * kernel's mix ring, at the master output volume, and only when their
 * setting (the snd.* preferences) allows. There are two voices: a click
 * (taps, keys, swipes, lock) and an alert (a tone, which for the timer
 * repeats until stopped or a minute passes). System sounds also drive the
 * separate piezo PWM with monophonic patterns, using the same switches,
 * tone choices and master volume. Arbitrary PCM retains its headphone path.
 */
#ifndef GLAZE_AUDIO_H
#define GLAZE_AUDIO_H

#include <glaze/abi.h>

#define AUDIO_PORT_NAME     "audio"
#define AUDIO_BUF_SIZE      8192u
#define AUDIO_PATH_MAX      128

/* Requests: w0 = op, w1 = session; replies: w0 = result (>= 0 or error). */
enum {
    AUD_HELLO = 1,          /* w1 = pid -> w1 = session, w2 = shm id */
    AUD_PLAY,               /* buf: path */
    AUD_PAUSE,
    AUD_RESUME,
    AUD_STOP,
    AUD_SEEK,               /* w2 = ms */
    AUD_NEXT,
    AUD_PREV,               /* restarts the track if more than 3 s in */
    AUD_STATE,              /* -> buf: struct audio_state */
    AUD_STREAM_OPEN,        /* w2 = rate, w3 = channels */
    AUD_STREAM_WRITE,       /* w2 = bytes of PCM in buf -> bytes accepted;
                               0 bytes: no more for now, start playing */
    AUD_STREAM_CLOCK,       /* -> w1 = ms played, w2 = ms queued */
    AUD_STREAM_CLOSE,
    AUD_SOUND_BUSY = 0x40,  /* no session -> 1 while a sound plays */
};

/* System sounds. AUDIO_SND_TONE + n previews tone n once, whatever the
 * switches say. Each is notification bit 1 << AUDIO_SND_*. */
enum {
    AUDIO_SND_TAP, AUDIO_SND_KEY, AUDIO_SND_SWIPE, AUDIO_SND_LOCK, AUDIO_SND_UNLOCK,
    AUDIO_SND_NOTIFY, AUDIO_SND_CALENDAR, AUDIO_SND_TIMER,
    AUDIO_SND_TONE = 8,     /* to 8 + AUDIO_TONES - 1 */
};
#define AUDIO_NOTIFY_SOUNDS     0x0000FFFFu
#define AUDIO_NOTIFY_STOP       0x00010000u     /* silences the alert voice */
#define AUDIO_NOTIFY_RELOAD     0x00020000u     /* re-reads the snd.* preferences */

/* Preferences (int32 each). Volume controls music, streams and effects.
 * Tones are an index below AUDIO_TONES, or
 * AUDIO_TONE_NONE. */
#define AUDIO_PREF_VOLUME       "snd.volume"        /* 0-10, default 7 */
#define AUDIO_PREF_TOUCH        "snd.touch"         /* default off */
#define AUDIO_PREF_KEYS         "snd.keys"          /* default on */
#define AUDIO_PREF_SWIPE        "snd.swipe"         /* default on */
#define AUDIO_PREF_LOCK         "snd.lock"          /* default on */
#define AUDIO_PREF_TONE_TIMER   "snd.tone.timer"
#define AUDIO_PREF_TONE_CAL     "snd.tone.calendar"
#define AUDIO_PREF_TONE_NOTIFY  "snd.tone.notify"
#define AUDIO_VOLUME_MAX        10
#define AUDIO_VOLUME_DEFAULT    7
#define AUDIO_TONES             5
#define AUDIO_TONE_NONE         (-1)

/* Tone n's name, and its clip in romfs "sounds/". */
static inline const char *audio_tone_name(int n)
{
    switch (n) {
    case 0: return "Tri-tone";
    case 1: return "Chime";
    case 2: return "Bell";
    case 3: return "Marimba";
    case 4: return "Alarm";
    }
    return "None";
}

static inline const char *audio_tone_file(int n)
{
    switch (n) {
    case 0: return "tritone.wav";
    case 1: return "chime.wav";
    case 2: return "bell.wav";
    case 3: return "marimba.wav";
    case 4: return "alarm.wav";
    }
    return NULL;
}

/* Default tones for the timer, calendar alerts and other notifications. */
#define AUDIO_TONE_TIMER_DEFAULT    4
#define AUDIO_TONE_CAL_DEFAULT      1
#define AUDIO_TONE_NOTIFY_DEFAULT   0

enum { AUDIO_STOPPED, AUDIO_PLAYING, AUDIO_PAUSED };

struct audio_state {
    uint32_t state;             /* AUDIO_* of the player */
    uint32_t position_ms, duration_ms;
    uint32_t rate, channels;    /* of the file */
    uint32_t bitrate;           /* kbit/s, MP3 only */
    uint32_t underruns;         /* since boot */
    uint32_t streaming;         /* a stream client holds the output */
    char path[AUDIO_PATH_MAX];  /* empty when nothing is loaded */
    char title[64], artist[64]; /* tags, or the file name and "" */
};

/* ---- client API (libc) --------------------------------------------------- */

int audio_play(const char *path);
int audio_pause(void);
int audio_resume(void);
int audio_stop(void);
int audio_seek(uint32_t ms);
int audio_next(void);
int audio_prev(void);
int audio_get_state(struct audio_state *st);

int audio_stream_open(uint32_t rate, int channels);
/* Bytes of `pcm` accepted (whole frames, at most AUDIO_BUF_SIZE); 0 when
 * the server's buffer is full. GZ_EBUSY once the player took over. Output
 * starts once a little is buffered, or at a write of 0 bytes. The clock
 * counts what has been heard, so it stands still while nothing plays. */
int audio_stream_write(const void *pcm, uint32_t len);
int audio_stream_clock(uint32_t *played_ms, uint32_t *queued_ms);
int audio_stream_close(void);

/* System sounds; these never open a session or wait for the server.
 * audio_sound_reload is for Settings, after it changes a snd.* preference.
 * audio_sound_busy (which does wait) is 1 while a sound plays. */
int audio_sound(int snd);
int audio_sound_stop(void);
int audio_sound_reload(void);
int audio_sound_busy(void);

/* Audio file extensions the player handles (.mp3, .wav). */
bool audio_is_playable(const char *name);

#endif
