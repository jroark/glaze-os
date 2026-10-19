/*
 * Audio server: owns the wave output (SYS_AUDIO_*, PROC_AUDIO) and serves
 * glaze/audio.h.
 *
 * One thread does everything. The kernel buffers GZ_AUDIO_RING bytes of PCM
 * and notifies this port (GZ_NOTIFY_AUDIO) when that drops below half, so
 * the loop decodes until the ring is full, then blocks in gz_recv until a
 * request or the notification arrives. Decoding goes one MP3 frame or WAV
 * chunk at a time with a non-blocking receive in between, which keeps
 * requests answered while the ring fills.
 *
 * Positions come from the kernel's count of frames played (at the hardware
 * rate) since the track started or was sought, so they follow what is
 * heard rather than what was decoded.
 */

#include <glaze/audio.h>
#include <glaze/fs.h>
#include <glaze/media.h>
#include <glaze/sys.h>
#include <mp3dec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_SESSIONS    4
#define IN_SIZE         8192            /* MP3 input buffer */
#define PCM_FRAMES      4096            /* decoded frames per unit, at most */
#define PEND_FRAMES     8192            /* converted frames waiting for the ring */
#define RAW_SIZE        2048            /* WAV bytes per unit; the largest ADPCM block */
#define PRIME_BYTES     16384           /* queued before the output starts */
#define PAGE_BYTES      4096            /* the kernel's DMA buffer size */

struct session {
    int pid;                /* 0 = free */
    int shm;
    uint8_t *buf;
};

enum { SRC_NONE, SRC_MP3, SRC_WAV };

static int port;
static struct session sessions[MAX_SESSIONS];

/* Output: what the hardware was started with. */
static uint32_t hw_rate, hw_ch;
static bool hw_running;
static int16_t pend[PEND_FRAMES * 2];
static uint32_t pend_off, pend_len;     /* in samples */
static struct media_resampler rs;

/* Player. */
static uint32_t state = AUDIO_STOPPED;
static char path[AUDIO_PATH_MAX];
static char title[64], artist[64];
static int src = SRC_NONE;
static int fd = -1;
static uint32_t file_size;
static bool src_eof;
static uint32_t seek_base_ms, played_base;
static uint32_t duration_ms, src_rate, src_ch, src_kbps;
static struct media_mp3_info mp3;
static HMP3Decoder dec;
static uint8_t in_buf[IN_SIZE];
static uint32_t in_pos, in_len, file_pos;
static struct media_wav wav;
static uint32_t wav_pos;                /* bytes into the data chunk */
static int16_t pcm[PCM_FRAMES * 2];
static uint8_t raw[RAW_SIZE];
static uint64_t drain_until;            /* end of track: last DMA buffers */
static bool reseek;                     /* a stream flushed our queued audio */
static uint32_t reseek_ms;

/* Stream. */
static int stream_session = -1;
static int stream_pid;
static uint32_t stream_rate, stream_ch;
static uint32_t stream_base;

static void log_msg(const char *msg)
{
    char line[128];

    snprintf(line, sizeof(line), "audio: %s\n", msg);
    gz_log(line, (uint32_t)strlen(line));
}

static void status(struct gz_audio_status *st)
{
    if (gz_audio_status(st) < 0)
        memset(st, 0, sizeof(*st));
}

/* ---- output ---------------------------------------------------------------- */

static void out_stop(bool flush)
{
    gz_audio_stop(flush);
    hw_running = false;
    if (flush)
        pend_off = pend_len = 0;
}

/* Set up conversion from `rate`/`ch` and flush everything queued. */
static void out_config(uint32_t rate, uint32_t ch)
{
    out_stop(true);
    hw_rate = media_hw_rate(rate);
    hw_ch = ch;
    media_rs_init(&rs, rate, (int)ch, hw_rate, (int)ch);
}

static void out_start(void)
{
    if (!hw_running && gz_audio_start(hw_rate, hw_ch) == 0)
        hw_running = true;
}

/* Moves pending samples into the kernel ring; true once they are all in. */
static bool out_flush_pend(void)
{
    while (pend_len) {
        int n = gz_audio_write(pend + pend_off, pend_len * 2);
        if (n <= 0)
            return false;
        pend_off += (uint32_t)n / 2;
        pend_len -= (uint32_t)n / 2;
    }
    pend_off = 0;
    return true;
}

/* Converts `frames` frames into pend; returns the frames consumed. */
static int out_convert(const int16_t *in, int frames)
{
    int used;
    int n = media_rs_run(&rs, in, frames, pend + pend_len, (int)(PEND_FRAMES - pend_len / hw_ch), &used);

    pend_len += (uint32_t)n * hw_ch;
    return used;
}

static uint32_t played_ms(uint32_t base)
{
    struct gz_audio_status st;

    status(&st);
    return hw_rate ? (uint32_t)((uint64_t)(st.frames_played - base) * 1000 / hw_rate) : 0;
}

/* ---- player sources ---------------------------------------------------------- */

static int fs_reader(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    (void)ctx;
    if (fs_seek(fd, (int32_t)off, FS_SEEK_SET) < 0)
        return GZ_EIO;
    return fs_read(fd, buf, len);
}

static void src_close(void)
{
    if (fd >= 0)
        fs_close(fd);
    fd = -1;
    src = SRC_NONE;
    src_eof = true;
}

static void base_name(const char *p, char *out, size_t max)
{
    const char *s = strrchr(p, '/');
    s = s ? s + 1 : p;
    snprintf(out, max, "%s", s);
    char *dot = strrchr(out, '.');
    if (dot && dot != out)
        *dot = 0;
}

static int src_open(const char *p)
{
    struct fs_stat st;
    const char *dot = strrchr(p, '.');

    src_close();
    if (fs_stat(p, &st) < 0 || st.type != FS_FILE)
        return GZ_ENOENT;
    fd = fs_open(p, FS_O_READ);
    if (fd < 0)
        return fd;
    file_size = st.size;
    title[0] = artist[0] = 0;
    if (dot && !strcasecmp(dot, ".mp3")) {
        if (media_mp3_probe(fs_reader, NULL, file_size, &mp3) < 0)
            goto bad;
        if (!dec && !(dec = MP3InitDecoder()))
            goto bad;
        src = SRC_MP3;
        src_rate = mp3.rate;
        src_ch = mp3.channels;
        src_kbps = mp3.bitrate;
        duration_ms = mp3.duration_ms;
        snprintf(title, sizeof(title), "%s", mp3.title);
        snprintf(artist, sizeof(artist), "%s", mp3.artist);
    } else if (dot && !strcasecmp(dot, ".wav")) {
        if (media_wav_open(fs_reader, NULL, file_size, &wav) < 0 || wav.fmt.block_align > RAW_SIZE)
            goto bad;
        src = SRC_WAV;
        src_rate = wav.fmt.rate;
        src_ch = wav.fmt.channels;
        src_kbps = 0;
        duration_ms = (uint32_t)((uint64_t)media_fmt_frames(&wav.fmt, wav.data_len) * 1000 / src_rate);
    } else {
        goto bad;
    }
    if (!title[0])
        base_name(p, title, sizeof(title));
    snprintf(path, sizeof(path), "%s", p);
    return 0;
bad:
    src_close();
    return GZ_EINVAL;
}

/* Positions the source at `ms`; returns the position actually reached. */
static uint32_t src_seek(uint32_t ms)
{
    src_eof = false;
    drain_until = 0;
    if (src == SRC_MP3) {
        file_pos = media_mp3_seek_offset(&mp3, ms);
        in_pos = in_len = 0;
        if (file_pos >= mp3.audio_end)
            src_eof = true;
        return ms;
    }
    uint32_t frame = (uint32_t)((uint64_t)ms * src_rate / 1000);
    wav_pos = MIN(media_fmt_offset(&wav.fmt, frame), wav.data_len);
    frame = wav_pos / wav.fmt.block_align * wav.fmt.block_frames;
    return (uint32_t)((uint64_t)frame * 1000 / src_rate);
}

/* Decodes one unit into pcm; returns frames, 0 at the end. */
static int src_decode(void)
{
    if (src == SRC_MP3) {
        for (;;) {
            if (in_len - in_pos < 2048 && file_pos < mp3.audio_end) {
                memmove(in_buf, in_buf + in_pos, in_len - in_pos);
                in_len -= in_pos;
                in_pos = 0;
                uint32_t want = MIN(IN_SIZE - in_len, mp3.audio_end - file_pos);
                int n = fs_reader(NULL, file_pos, in_buf + in_len, want);
                if (n <= 0) {
                    file_pos = mp3.audio_end;
                } else {
                    in_len += (uint32_t)n;
                    file_pos += (uint32_t)n;
                }
            }
            int left = (int)(in_len - in_pos);
            if (left <= 0)
                return 0;
            int sync = MP3FindSyncWord(in_buf + in_pos, left);
            if (sync < 0) {
                in_pos = in_len;
                if (file_pos >= mp3.audio_end)
                    return 0;
                continue;
            }
            in_pos += (uint32_t)sync;
            unsigned char *p = in_buf + in_pos;
            int avail = (int)(in_len - in_pos);
            int err = MP3Decode(dec, &p, &avail, pcm, 0);
            if (err == ERR_MP3_INDATA_UNDERFLOW) {
                if (file_pos >= mp3.audio_end)
                    return 0;
                if (avail >= 2048)
                    in_pos++;       /* too long for a frame: a false sync */
                continue;
            }
            if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
                in_pos = (uint32_t)(p - in_buf);   /* bit reservoir after a seek */
                continue;
            }
            if (err) {
                in_pos++;
                continue;
            }
            in_pos = (uint32_t)(p - in_buf);
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(dec, &fi);
            if (fi.nChans != (int)src_ch || fi.samprate != (int)src_rate)
                continue;           /* a stray frame from another stream */
            return fi.outputSamps / fi.nChans;
        }
    }
    if (src == SRC_WAV) {
        uint32_t unit = wav.fmt.format == MEDIA_FMT_IMA ? wav.fmt.block_align
                      : RAW_SIZE / wav.fmt.block_align * wav.fmt.block_align;
        unit = MIN(unit, wav.data_len - wav_pos);
        if (!unit)
            return 0;
        int n = fs_reader(NULL, wav.data_off + wav_pos, raw, unit);
        if (n <= 0)
            return 0;
        wav_pos += (uint32_t)n;
        return media_decode(&wav.fmt, raw, (uint32_t)n, pcm);
    }
    return 0;
}

/* Decodes one unit into the ring, once what is pending has gone in. A unit
 * is at most PCM_FRAMES, and the hardware rate is within 10% of the source
 * rate, so it always converts into an empty pend. */
static void pump_one(void)
{
    if (!out_flush_pend() || src_eof)
        return;
    int frames = src_decode();
    if (frames <= 0) {
        src_eof = true;
        return;
    }
    out_convert(pcm, frames);
    out_flush_pend();
}

static bool ring_wants_data(void)
{
    struct gz_audio_status st;

    status(&st);
    return st.queued + pend_len * 2 < GZ_AUDIO_RING - 4096;
}

/* Fills the ring until PRIME_BYTES are queued, then starts the output. */
static void prime_and_start(void)
{
    struct gz_audio_status st;

    for (uint32_t last = ~0u;;) {
        status(&st);
        if (src_eof || st.queued >= PRIME_BYTES || (st.queued == last && pend_len))
            break;
        last = st.queued;
        pump_one();
    }
    status(&st);
    played_base = st.frames_played;
    out_start();
}

/* ---- playlist ---------------------------------------------------------------- */

static int cmp_names(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

/* The playable file `delta` places from the current one in its folder. */
static bool neighbour(int delta, char *out, size_t max)
{
    static char names[64][FS_NAME_MAX];
    struct fs_dirent ents[8];
    char dir[AUDIO_PATH_MAX];
    int count = 0, n;
    const char *slash = strrchr(path, '/');

    if (!slash)
        return false;
    snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);
    for (uint32_t skip = 0; count < 64 && (n = fs_list(dir[0] ? dir : "/", skip, ents, 8)) > 0; skip += (uint32_t)n)
        for (int i = 0; i < n && count < 64; i++)
            if (ents[i].type == FS_FILE && audio_is_playable(ents[i].name))
                snprintf(names[count++], FS_NAME_MAX, "%s", ents[i].name);
    qsort(names, (size_t)count, FS_NAME_MAX, cmp_names);
    for (int i = 0; i < count; i++) {
        if (strcasecmp(names[i], slash + 1))
            continue;
        if (i + delta < 0 || i + delta >= count)
            return false;
        snprintf(out, max, "%s/%s", dir, names[i + delta]);
        return true;
    }
    return false;
}

/* ---- player operations --------------------------------------------------------- */

static int play_at(const char *p, uint32_t ms)
{
    if (stream_session >= 0) {
        out_stop(true);
        stream_session = -1;
    }
    int rv = src_open(p);
    if (rv < 0) {
        state = AUDIO_STOPPED;
        return rv;
    }
    out_config(src_rate, src_ch);
    seek_base_ms = src_seek(ms);
    reseek = false;
    state = AUDIO_PLAYING;
    prime_and_start();
    return 0;
}

static uint32_t position(void)
{
    if (src == SRC_NONE)
        return 0;
    if (reseek)
        return reseek_ms;
    return MIN(seek_base_ms + played_ms(played_base), duration_ms);
}

static int op_seek(uint32_t ms)
{
    if (src == SRC_NONE)
        return GZ_EINVAL;
    if (stream_session >= 0) {
        reseek_ms = MIN(ms, duration_ms);
        return 0;
    }
    out_stop(true);
    seek_base_ms = src_seek(MIN(ms, duration_ms));
    reseek = false;
    if (state == AUDIO_PLAYING) {
        prime_and_start();
    } else {
        struct gz_audio_status st;
        status(&st);
        played_base = st.frames_played;
    }
    return 0;
}

static int op_pause(void)
{
    if (state != AUDIO_PLAYING)
        return 0;
    if (stream_session < 0)
        out_stop(false);
    state = AUDIO_PAUSED;
    return 0;
}

static int op_resume(void)
{
    if (src == SRC_NONE)
        return GZ_EINVAL;
    if (state == AUDIO_PLAYING)
        return 0;
    if (stream_session >= 0) {
        out_stop(true);
        stream_session = -1;
        reseek = true;
    }
    if (state == AUDIO_STOPPED || reseek) {
        uint32_t ms = state == AUDIO_STOPPED ? 0 : reseek_ms;
        return play_at(path, ms);
    }
    state = AUDIO_PLAYING;
    out_start();
    return 0;
}

static int op_stop(void)
{
    out_stop(true);
    if (src != SRC_NONE) {
        src_seek(0);
        seek_base_ms = 0;
    }
    reseek = false;
    state = AUDIO_STOPPED;
    struct gz_audio_status st;
    status(&st);
    played_base = st.frames_played;
    return 0;
}

static int op_skip(int delta)
{
    char next[AUDIO_PATH_MAX];

    if (src == SRC_NONE)
        return GZ_EINVAL;
    if (delta < 0 && position() > 3000)
        return play_at(path, 0);
    if (!neighbour(delta, next, sizeof(next)))
        return delta < 0 ? play_at(path, 0) : GZ_ENOENT;
    bool paused = state == AUDIO_PAUSED;
    int rv = play_at(next, 0);
    if (!rv && paused)
        op_pause();
    return rv;
}

/* Called every loop: decode while there is room, advance at the end. */
static void player_service(void)
{
    if (state != AUDIO_PLAYING || stream_session >= 0 || src == SRC_NONE)
        return;
    if (!src_eof || pend_len) {
        if (ring_wants_data())
            pump_one();
        return;
    }
    struct gz_audio_status st;
    status(&st);
    if (st.queued)
        return;
    /* Everything decoded has gone to the DMA; let its two buffers play. */
    uint64_t now = gz_time_us();
    if (!drain_until)
        drain_until = now + 2ull * PAGE_BYTES / (2 * hw_ch) * 1000000 / hw_rate + 20000;
    if (now < drain_until)
        return;
    char next[AUDIO_PATH_MAX];
    if (neighbour(1, next, sizeof(next)) && play_at(next, 0) == 0)
        return;
    op_stop();
}

/* ---- stream ---------------------------------------------------------------- */

static int stream_open(int sess, uint32_t rate, uint32_t ch)
{
    if (rate < 4000 || rate > 96000 || (ch != 1 && ch != 2))
        return GZ_EINVAL;
    if (stream_session < 0 && src != SRC_NONE && state != AUDIO_STOPPED) {
        reseek_ms = position();
        reseek = true;
        if (state == AUDIO_PLAYING)
            state = AUDIO_PAUSED;
    }
    stream_session = sess;
    stream_pid = sessions[sess].pid;
    stream_rate = rate;
    stream_ch = ch;
    out_config(rate, ch);
    struct gz_audio_status st;
    status(&st);
    stream_base = st.frames_played;
    return 0;
}

static void stream_close(void)
{
    if (stream_session < 0)
        return;
    stream_session = -1;
    out_stop(true);
}

static int stream_write(struct session *s, uint32_t len)
{
    const int16_t *in = (const int16_t *)s->buf;
    int frames = (int)(MIN(len, AUDIO_BUF_SIZE) / (2 * stream_ch));
    int done = 0;

    if (!out_flush_pend())
        return 0;
    while (done < frames) {
        int used = out_convert(in + done * (int)stream_ch, frames - done);
        done += used;
        if (!out_flush_pend() || !used)
            break;
    }
    /* Start once there is something to play, like the player's priming, or
     * at a zero-length write, which marks the end of the data. */
    struct gz_audio_status st;
    status(&st);
    if (!hw_running && st.queued && (st.queued >= PRIME_BYTES || !len)) {
        stream_base = st.frames_played;
        out_start();
    }
    return done * 2 * (int)stream_ch;
}

static int stream_clock(struct gz_msg *r)
{
    struct gz_audio_status st;

    status(&st);
    r->w[1] = hw_rate ? (uint32_t)((uint64_t)(st.frames_played - stream_base) * 1000 / hw_rate) : 0;
    r->w[2] = hw_rate ? (uint32_t)((uint64_t)(st.queued / 2 + pend_len) / hw_ch * 1000 / hw_rate) : 0;
    return 0;
}

/* ---- sessions and dispatch ------------------------------------------------------- */

static int op_hello(int pid, struct gz_msg *r)
{
    int pick = -1;

    for (int i = 0; i < MAX_SESSIONS && pick < 0; i++)
        if (sessions[i].pid == pid)
            pick = i;
    for (int i = 0; i < MAX_SESSIONS && pick < 0; i++)
        if (!sessions[i].pid || !gz_proc_alive(sessions[i].pid))
            pick = i;
    if (pick < 0)
        return GZ_EFULL;
    struct session *s = &sessions[pick];
    if (stream_session == pick)
        stream_close();
    if (!s->buf) {
        s->shm = gz_shm_create(AUDIO_BUF_SIZE);
        s->buf = s->shm > 0 ? gz_shm_map(s->shm) : NULL;
        if (!s->buf)
            return GZ_ENOMEM;
    }
    s->pid = pid;
    r->w[1] = (uint32_t)pick;
    r->w[2] = (uint32_t)s->shm;
    return 0;
}

static int op_state(struct session *s)
{
    struct audio_state *st = (struct audio_state *)s->buf;
    struct gz_audio_status ks;

    status(&ks);
    memset(st, 0, sizeof(*st));
    st->state = src == SRC_NONE ? AUDIO_STOPPED : state;
    st->position_ms = position();
    st->duration_ms = src == SRC_NONE ? 0 : duration_ms;
    st->rate = src_rate;
    st->channels = src_ch;
    st->bitrate = src_kbps;
    st->underruns = ks.underruns;
    st->streaming = stream_session >= 0;
    if (src != SRC_NONE) {
        snprintf(st->path, sizeof(st->path), "%s", path);
        snprintf(st->title, sizeof(st->title), "%s", title);
        snprintf(st->artist, sizeof(st->artist), "%s", artist);
    }
    return 0;
}

static int dispatch(struct gz_msg *m, struct gz_msg *r)
{
    uint32_t op = m->w[0], sess = m->w[1];

    if (op == AUD_HELLO)
        return op_hello((int)m->w[1], r);
    if (sess >= MAX_SESSIONS || !sessions[sess].pid)
        return GZ_EINVAL;
    struct session *s = &sessions[sess];
    switch (op) {
    case AUD_PLAY:
        if (!memchr(s->buf, 0, AUDIO_PATH_MAX))
            return GZ_EINVAL;
        return play_at((const char *)s->buf, 0);
    case AUD_PAUSE:         return op_pause();
    case AUD_RESUME:        return op_resume();
    case AUD_STOP:          return op_stop();
    case AUD_SEEK:          return op_seek(m->w[2]);
    case AUD_NEXT:          return op_skip(1);
    case AUD_PREV:          return op_skip(-1);
    case AUD_STATE:         return op_state(s);
    case AUD_STREAM_OPEN:   return stream_open((int)sess, m->w[2], m->w[3]);
    case AUD_STREAM_WRITE:
        if (stream_session != (int)sess)
            return GZ_EBUSY;
        return stream_write(s, m->w[2]);
    case AUD_STREAM_CLOCK:
        if (stream_session != (int)sess)
            return GZ_EBUSY;
        return stream_clock(r);
    case AUD_STREAM_CLOSE:
        if (stream_session == (int)sess)
            stream_close();
        return 0;
    }
    return GZ_EINVAL;
}

int main(void)
{
    struct gz_msg m, r;
    uint64_t last_reap = 0;

    port = gz_port_create();
    if (gz_audio_bind(port) < 0) {
        log_msg("no wave output");
        return 1;
    }
    gz_port_register(port, AUDIO_PORT_NAME);
    for (;;) {
        int timeout = 1000;
        if (state == AUDIO_PLAYING && stream_session < 0 && src != SRC_NONE)
            timeout = (!src_eof || pend_len) ? (ring_wants_data() ? 0 : -1) : 50;
        int t = gz_recv(port, &m, timeout);
        if (t > 0) {
            memset(&r, 0, sizeof(r));
            r.w[0] = (uint32_t)dispatch(&m, &r);
            gz_reply(t, &r);
        }
        player_service();
        uint64_t now = gz_time_us();
        if (now - last_reap > 1000000) {
            last_reap = now;
            if (stream_session >= 0 && !gz_proc_alive(stream_pid))
                stream_close();
        }
    }
}
