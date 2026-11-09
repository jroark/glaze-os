/* Wave output: waveOut* on the audio server's PCM stream (glaze/audio.h).
 * One device. A feeder thread writes each queued WAVEHDR's data in order
 * (8-bit samples widened to 16), marks it done once the server has taken
 * it, and reports WOM_DONE through the app's CALLBACK_WINDOW, _THREAD,
 * _FUNCTION or _EVENT. Contracts: Windows CE 3.0 waveOutOpen/Write/Reset
 * and WAVEHDR. The ACM's conversion streams aren't provided: as on a CE
 * device without the codec, acmStreamOpen reports ACMERR_NOTPOSSIBLE. */
#include "runtime.h"
#include <glaze/audio.h>
#include <glaze/thread.h>

#define WOM_OPEN 0x3bb
#define WOM_CLOSE 0x3bc
#define WOM_DONE 0x3bd
#define WHDR_DONE 1
#define WHDR_PREPARED 2
#define WHDR_INQUEUE 0x10
#define DEVICE 0x7e100001u
#define QUEUE 64
CE_API(SetEvent);

static struct gz_mutex lock = GZ_MUTEX_INIT;
static bool open_now, paused, feeding;
static CW callback, instance, flags;
static unsigned channels, bits, block_align, rate, bytes_per_second;
static CW queue[QUEUE];
static unsigned head, count, offset; /* offset into queue[head]'s data */
static uint64_t written;             /* bytes handed to the server */
static uint32_t base_ms;             /* stream clock at the last reset */

static void notify(CW message, CW header)
{
    switch (flags & 0x70000) {
    case 0x10000: /* CALLBACK_WINDOW */
        ce_post(callback, message, DEVICE, header);
        break;
    case 0x20000: /* CALLBACK_THREAD */
        ce_post(0, message, DEVICE, header);
        break;
    case 0x30000: /* CALLBACK_FUNCTION(device, message, instance, param1, param2) */
        if (callback)
            ce_call(callback, DEVICE, message, instance, header);
        break;
    case 0x50000: /* CALLBACK_EVENT */
        ce_SetEvent(callback, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        break;
    }
}
static void finish_head(void)
{
    CW h = queue[head];
    uint8_t *hdr = PTR(uint8_t, h);
    ce_put32(hdr + 16, (ce_u32(hdr + 16) & ~(CW)WHDR_INQUEUE) | WHDR_DONE);
    head = (head + 1) % QUEUE;
    count--;
    offset = 0;
    gz_mutex_unlock(&lock);
    notify(WOM_DONE, h);
    gz_mutex_lock(&lock);
}
static void feed(void *arg)
{
    (void)arg;
    int16_t pcm[2048];
    gz_mutex_lock(&lock);
    while (open_now) {
        if (paused || !count) {
            gz_mutex_unlock(&lock);
            gz_sleep_ms(5);
            gz_mutex_lock(&lock);
            continue;
        }
        const uint8_t *hdr = PTR(uint8_t, queue[head]);
        const uint8_t *data = PTR(uint8_t, ce_u32(hdr));
        uint32_t length = ce_u32(hdr + 4);
        if (offset >= length || !data) {
            finish_head();
            continue;
        }
        uint32_t take = MIN(length - offset, (uint32_t)sizeof(pcm) / (bits == 8 ? 2 : 1));
        take -= take % block_align;
        if (!take) {
            finish_head();
            continue;
        }
        const void *out = data + offset;
        uint32_t out_bytes = take;
        if (bits == 8) {
            for (uint32_t i = 0; i < take; i++)
                pcm[i] = (int16_t)((data[offset + i] - 128) << 8);
            out = pcm;
            out_bytes = take * 2;
        }
        int n = audio_stream_write(out, out_bytes);
        if (n <= 0) {
            gz_mutex_unlock(&lock);
            gz_sleep_ms(n < 0 ? 50 : 5);
            gz_mutex_lock(&lock);
            continue;
        }
        uint32_t used = bits == 8 ? (uint32_t)n / 2 : (uint32_t)n;
        offset += used;
        written += used;
    }
    feeding = false;
    gz_mutex_unlock(&lock);
}
CE_API(waveOutGetNumDevs) { return 1; }
/* waveOutOpen(device out, id, WAVEFORMATEX, callback, instance, flags):
 * PCM, 8 or 16 bits, mono or stereo; WAVE_FORMAT_QUERY (1) only checks. */
CE_API(waveOutOpen)
{
    const uint8_t *fmt = PTR(uint8_t, a2);
    if (!fmt)
        return 11; /* MMSYSERR_INVALPARAM */
    unsigned tag = ce_u16(fmt), ch = ce_u16(fmt + 2), bps = ce_u16(fmt + 14);
    uint32_t hz = ce_u32(fmt + 4);
    if (tag != 1 || (ch != 1 && ch != 2) || (bps != 8 && bps != 16) || hz < 4000 || hz > 48000)
        return 32; /* WAVERR_BADFORMAT */
    if (a5 & 1)
        return 0;
    gz_mutex_lock(&lock);
    if (open_now || feeding) {
        gz_mutex_unlock(&lock);
        return 4; /* MMSYSERR_ALLOCATED */
    }
    if (audio_stream_open(hz, (int)ch)) {
        gz_mutex_unlock(&lock);
        return 1; /* MMSYSERR_ERROR */
    }
    channels = ch;
    bits = bps;
    rate = hz;
    block_align = ch * bps / 8;
    bytes_per_second = hz * block_align;
    callback = a3;
    instance = a4;
    flags = a5;
    paused = false;
    head = count = offset = 0;
    written = 0;
    base_ms = 0;
    open_now = feeding = true;
    gz_mutex_unlock(&lock);
    if (gz_thread_create(feed, NULL) < 0) {
        gz_mutex_lock(&lock);
        open_now = feeding = false;
        audio_stream_close();
        gz_mutex_unlock(&lock);
        return 7; /* MMSYSERR_NOMEM */
    }
    if (a0)
        *PTR(CW, a0) = DEVICE;
    notify(WOM_OPEN, 0);
    return 0;
}
CE_API(waveOutClose)
{
    if (a0 != DEVICE || !open_now)
        return 5; /* MMSYSERR_INVALHANDLE */
    gz_mutex_lock(&lock);
    if (count) {
        gz_mutex_unlock(&lock);
        return 33; /* WAVERR_STILLPLAYING */
    }
    open_now = false;
    gz_mutex_unlock(&lock);
    while (feeding)
        gz_sleep_ms(2);
    audio_stream_close();
    notify(WOM_CLOSE, 0);
    return 0;
}
CE_API(waveOutPrepareHeader)
{
    if (a0 != DEVICE || !a1)
        return a1 ? 5 : 11;
    uint8_t *hdr = PTR(uint8_t, a1);
    ce_put32(hdr + 16, ce_u32(hdr + 16) | WHDR_PREPARED);
    return 0;
}
CE_API(waveOutUnprepareHeader)
{
    if (a0 != DEVICE || !a1)
        return a1 ? 5 : 11;
    uint8_t *hdr = PTR(uint8_t, a1);
    if (ce_u32(hdr + 16) & WHDR_INQUEUE)
        return 33;
    ce_put32(hdr + 16, ce_u32(hdr + 16) & ~(CW)WHDR_PREPARED);
    return 0;
}
CE_API(waveOutWrite)
{
    if (a0 != DEVICE || !open_now)
        return 5;
    uint8_t *hdr = PTR(uint8_t, a1);
    if (!hdr || !(ce_u32(hdr + 16) & WHDR_PREPARED))
        return 34; /* WAVERR_UNPREPARED */
    gz_mutex_lock(&lock);
    if (count == QUEUE) {
        gz_mutex_unlock(&lock);
        return 7;
    }
    ce_put32(hdr + 16, (ce_u32(hdr + 16) & ~(CW)WHDR_DONE) | WHDR_INQUEUE);
    queue[(head + count++) % QUEUE] = a1;
    gz_mutex_unlock(&lock);
    return 0;
}
CE_API(waveOutPause)
{
    if (a0 != DEVICE)
        return 5;
    paused = true;
    return 0;
}
CE_API(waveOutRestart)
{
    if (a0 != DEVICE)
        return 5;
    paused = false;
    audio_stream_write(NULL, 0); /* start what is buffered */
    return 0;
}
/* waveOutReset: every queued buffer comes back done; the position restarts. */
CE_API(waveOutReset)
{
    if (a0 != DEVICE)
        return 5;
    gz_mutex_lock(&lock);
    while (count)
        finish_head();
    uint32_t played = 0;
    audio_stream_clock(&played, NULL);
    base_ms = played;
    written = 0;
    paused = false;
    gz_mutex_unlock(&lock);
    return 0;
}
/* waveOutGetPosition(device, MMTIME, size): TIME_MS 1, TIME_SAMPLES 2,
 * TIME_BYTES 4 (others are answered in bytes), from the stream clock. */
CE_API(waveOutGetPosition)
{
    if (a0 != DEVICE || !a1)
        return 5;
    uint32_t played = 0;
    gz_mutex_lock(&lock);
    audio_stream_clock(&played, NULL);
    uint64_t ms = played >= base_ms ? played - base_ms : 0;
    uint64_t bytes = MIN(ms * bytes_per_second / 1000, written);
    gz_mutex_unlock(&lock);
    CW *t = PTR(CW, a1);
    if (t[0] == 1)
        t[1] = (CW)(bytes * 1000 / MAX(bytes_per_second, 1u));
    else if (t[0] == 2)
        t[1] = (CW)(bytes / MAX(block_align, 1u));
    else {
        t[0] = 4;
        t[1] = (CW)bytes;
    }
    return 0;
}
CE_API(acmStreamOpen)
{
    if (a0)
        *PTR(CW, a0) = 0;
    return 512; /* ACMERR_NOTPOSSIBLE */
}
CE_API(acmFormatSuggest) { return 512; }
CE_API(acmStreamClose) { return 5; }

static const struct ce_export exports[] = {
    CE_EXPORT(waveOutGetNumDevs), CE_EXPORT(waveOutOpen), CE_EXPORT(waveOutClose), CE_EXPORT(waveOutPrepareHeader),
    CE_EXPORT(waveOutUnprepareHeader), CE_EXPORT(waveOutWrite), CE_EXPORT(waveOutPause), CE_EXPORT(waveOutRestart),
    CE_EXPORT(waveOutReset), CE_EXPORT(waveOutGetPosition), CE_EXPORT(acmStreamOpen), CE_EXPORT(acmFormatSuggest),
    CE_EXPORT(acmStreamClose), {"acmStreamConvert", (void *)ce_acmStreamClose},
    {"acmStreamPrepareHeader", (void *)ce_acmStreamClose}, {"acmStreamUnprepareHeader", (void *)ce_acmStreamClose},
    {"acmStreamReset", (void *)ce_acmStreamClose}, {"acmStreamSize", (void *)ce_acmStreamClose},
};
void *ce_resolve_wave(const char *name)
{
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++)
        if (!strcmp(name, exports[i].name))
            return exports[i].address;
    return NULL;
}
