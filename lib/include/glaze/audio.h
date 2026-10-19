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
};

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

/* Audio file extensions the player handles (.mp3, .wav). */
bool audio_is_playable(const char *name);

#endif
