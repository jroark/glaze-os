/* Audio client: requests to the audio server (see glaze/audio.h). */

#include <glaze/audio.h>
#include <glaze/sys.h>
#include <glaze/thread.h>
#include <string.h>
#include <strings.h>

static int port, session = -1;
static uint8_t *buf;

/* One call at a time per process: the shared buffer is the session's. */
static struct gz_mutex session_lock = GZ_MUTEX_INIT;

static void release(struct gz_mutex **m)
{
    gz_mutex_unlock(*m);
}

#define SESSION_LOCKED() \
    struct gz_mutex *held_ __attribute__((cleanup(release))) = (gz_mutex_lock(&session_lock), &session_lock)

/* Connects on first use; the server starts with the other servers at boot. */
static int connect(void)
{
    struct gz_msg m, r;

    if (session >= 0)
        return 0;
    for (int tries = 0; (port = gz_port_lookup(AUDIO_PORT_NAME)) <= 0; tries++) {
        if (tries == 100)
            return GZ_ENOENT;
        gz_sleep_ms(50);
    }
    memset(&m, 0, sizeof(m));
    m.w[0] = AUD_HELLO;
    m.w[1] = (uint32_t)gz_getpid();
    if (gz_send(port, &m, &r) < 0 || (int)r.w[0] < 0)
        return r.w[0] ? (int)r.w[0] : GZ_EIO;
    buf = gz_shm_map((int)r.w[2]);
    if (!buf)
        return GZ_ENOMEM;
    session = (int)r.w[1];
    return 0;
}

static int call(uint32_t op, uint32_t a, uint32_t b, struct gz_msg *reply)
{
    struct gz_msg m, r;
    int rv = connect();

    if (rv)
        return rv;
    memset(&m, 0, sizeof(m));
    m.w[0] = op;
    m.w[1] = (uint32_t)session;
    m.w[2] = a;
    m.w[3] = b;
    if (gz_send(port, &m, &r) < 0)
        return GZ_EIO;
    if (reply)
        *reply = r;
    return (int)r.w[0];
}

int audio_play(const char *path)
{
    SESSION_LOCKED();
    size_t n = strlen(path);
    int rv = connect();

    if (rv)
        return rv;
    if (n >= AUDIO_PATH_MAX)
        return GZ_EINVAL;
    memcpy(buf, path, n + 1);
    return call(AUD_PLAY, 0, 0, NULL);
}

int audio_pause(void) { SESSION_LOCKED(); return call(AUD_PAUSE, 0, 0, NULL); }
int audio_resume(void) { SESSION_LOCKED(); return call(AUD_RESUME, 0, 0, NULL); }
int audio_stop(void) { SESSION_LOCKED(); return call(AUD_STOP, 0, 0, NULL); }
int audio_seek(uint32_t ms) { SESSION_LOCKED(); return call(AUD_SEEK, ms, 0, NULL); }
int audio_next(void) { SESSION_LOCKED(); return call(AUD_NEXT, 0, 0, NULL); }
int audio_prev(void) { SESSION_LOCKED(); return call(AUD_PREV, 0, 0, NULL); }

int audio_get_state(struct audio_state *st)
{
    SESSION_LOCKED();
    int rv = call(AUD_STATE, 0, 0, NULL);

    if (rv >= 0)
        memcpy(st, buf, sizeof(*st));
    return rv;
}

int audio_stream_open(uint32_t rate, int channels)
{
    SESSION_LOCKED();
    return call(AUD_STREAM_OPEN, rate, (uint32_t)channels, NULL);
}

int audio_stream_write(const void *pcm, uint32_t len)
{
    SESSION_LOCKED();
    int rv = connect();

    if (rv)
        return rv;
    len = MIN(len, AUDIO_BUF_SIZE);
    memcpy(buf, pcm, len);
    return call(AUD_STREAM_WRITE, len, 0, NULL);
}

int audio_stream_clock(uint32_t *played_ms, uint32_t *queued_ms)
{
    SESSION_LOCKED();
    struct gz_msg r = { { 0 } };
    int rv = call(AUD_STREAM_CLOCK, 0, 0, &r);

    if (rv >= 0) {
        if (played_ms)
            *played_ms = r.w[1];
        if (queued_ms)
            *queued_ms = r.w[2];
    }
    return rv;
}

int audio_stream_close(void) { SESSION_LOCKED(); return call(AUD_STREAM_CLOSE, 0, 0, NULL); }

bool audio_is_playable(const char *name)
{
    const char *dot = strrchr(name, '.');

    return dot && (!strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".wav"));
}
