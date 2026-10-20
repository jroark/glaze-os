/*
 * Wave output: the BE-300's ping-pong PCM DMA block.
 *
 * This is a Casio block in the Kj companion window, not the VRC4173 AIU of
 * UM chapter 10. Its register map and start/stop sequences are reverse-
 * engineered from WinCE's wavedev.dll (be300 docs/BE300_AUDIO.md) and are
 * modelled by the emulator (be300 src/hw/aiu.c). The DMA plays buffer 0,
 * then buffer 1, and so on; each time one drains it sets that buffer's bit
 * in AUD_STATUS, which raises GIRQ0 bit 8 while the source enable at
 * 0x0A000030 bit 3 is on, and moves to the other buffer.
 *
 * The kernel keeps a PCM ring the audio server (servers/audio, the one
 * process started with PROC_AUDIO) fills with SYS_AUDIO_WRITE. Each drained
 * buffer is refilled from the ring, padded with silence on underrun, and
 * the bound port is notified while the ring is below half full.
 *
 * Effects (UI sounds and alerts) go into a second, smaller ring that each
 * refill adds over the main ring's samples (or over the silence of an
 * underrun) with saturation, so they are heard within a buffer or two
 * however much music is queued. Both rings hold the output's format.
 *
 * Open questions from BE300_AUDIO.md, followed as wavedev does: how the
 * enable and ack bits split across 0x8C4/0x8C8/0x8CC, and whether the
 * buffer registers read back the live DMA address (they are never read
 * here). No speaker-amplifier GPIO is known.
 *
 * Buffers are filled through KSEG1, so the uncached writes reach the DMA
 * without cache maintenance (see the colouring TODO in mm.c).
 */

#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"

#define DMA_BYTES       PAGE_SIZE       /* per buffer; one page each */
#define RING_BYTES      GZ_AUDIO_RING
#define MIX_BYTES       GZ_AUDIO_MIX_RING
#define CODEC_SPINS     100000

static uint8_t *dma_buf[2];             /* KSEG1 aliases */
static uint32_t dma_frames[2];          /* ring frames each buffer holds */
static uint8_t ring[RING_BYTES];
static uint32_t ring_head, ring_tail;   /* free-running byte counts */
static uint8_t mix[MIX_BYTES];
static uint32_t mix_head, mix_tail;
static bool mix_used;                   /* a refill took effects since the last notify */

static bool present;
static bool running;
static bool idle;                       /* started for the mix ring only */
static int bound_port = -1;
static uint32_t cur_rate, cur_channels;
static uint32_t next_drain;             /* buffer the DMA finishes next */
static uint32_t frames_played, underruns;

/* Rate code for AUD_CTRL bits 6:4 and the codec double-rate bit, from
 * wavedev's selection at 0x019111DC (BE300_AUDIO.md, "Sample rate"). */
static int rate_setup(uint32_t rate, uint32_t *code, uint32_t *x2)
{
    switch (rate) {
    case 8000:  *code = 0x40; *x2 = 0; return 0;
    case 11025: *code = 0x10; *x2 = 0; return 0;
    case 16000: *code = 0x30; *x2 = 0; return 0;
    case 22050: *code = 0x00; *x2 = 0; return 0;
    case 32000: *code = 0x30; *x2 = 1; return 0;
    case 44100: *code = 0x00; *x2 = 1; return 0;
    }
    return GZ_EINVAL;
}

static void codec_cmd(uint32_t cmd)
{
    for (int i = 0; i < CODEC_SPINS && !AUD_CODEC_READY; i++)
        ;
    AUD_CODEC_CMD = cmd;
}

/* Adds up to `len` bytes (whole frames) of the mix ring onto `d`,
 * saturating. The ring only ever holds whole frames. */
static void mix_in(uint8_t *d, uint32_t len)
{
    uint32_t n = MIN(mix_head - mix_tail, len);
    int16_t *s = (int16_t *)d;

    for (uint32_t k = 0; k < n; k += 2) {
        uint32_t at = (mix_tail + k) % MIX_BYTES;
        int32_t v = s[k / 2] + (int16_t)(mix[at] | mix[at + 1] << 8);
        s[k / 2] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    mix_tail += n;
    mix_used |= n != 0;
}

/* Move up to one buffer of PCM from the ring into dma_buf[i]. */
static void refill(uint32_t i)
{
    uint32_t frame = 2 * cur_channels;
    uint32_t avail = (ring_head - ring_tail) / frame * frame;
    uint32_t n = MIN(avail, DMA_BYTES / frame * frame);
    uint8_t *d = dma_buf[i];

    for (uint32_t k = 0; k < n; k++)
        d[k] = ring[(ring_tail + k) % RING_BYTES];
    ring_tail += n;
    dma_frames[i] = n / frame;
    if (n < DMA_BYTES) {
        memset(d + n, 0, DMA_BYTES - n);
        if (running && !idle && n < DMA_BYTES / frame * frame)
            underruns++;
    }
    mix_in(d, DMA_BYTES / frame * frame);
}

static void notify_if_low(void)
{
    /* An empty mix ring is the usual case: only one being drained asks
     * for more. */
    bool mix_low = mix_used && mix_head - mix_tail < MIX_BYTES / 2;

    mix_used = false;
    if (bound_port >= 0 && (ring_head - ring_tail < RING_BYTES / 2 || mix_low))
        ipc_notify(bound_port, GZ_NOTIFY_AUDIO);
}

void audio_init(void)
{
    uint8_t *pages = page_alloc_n(2);

    if (!pages) {
        kputs("audio: no DMA memory\n");
        return;
    }
    dma_buf[0] = (uint8_t *)KSEG1(PHYS(pages));
    dma_buf[1] = (uint8_t *)KSEG1(PHYS(pages) + DMA_BYTES);
    AUD_BUF0_START = KSEG1(PHYS(dma_buf[0]));
    AUD_BUF0_END = KSEG1(PHYS(dma_buf[0])) + DMA_BYTES - 1;
    AUD_BUF1_START = KSEG1(PHYS(dma_buf[1]));
    AUD_BUF1_END = KSEG1(PHYS(dma_buf[1])) + DMA_BYTES - 1;
    /* The source is enabled at reset (0x0C, hw_dump_vrc4173.txt:27); keep
     * it on, since the drained-buffer status is what we acknowledge. */
    KJ_SRC_EN |= AUD_SRC_EN_BIT;
    present = true;
}

void audio_irq(void)
{
    uint32_t st;

    if (!running)
        return;
    st = AUD_STATUS & 3u;
    if (!st)
        return;
    /* wavedev's IST (0x0191274C) acknowledges with 0x11 to both. */
    AUD_ACK0 = 0x11;
    AUD_ACK1 = 0x11;
    while (st & (1u << next_drain)) {
        st &= ~(1u << next_drain);
        frames_played += dma_frames[next_drain];
        refill(next_drain);
        next_drain ^= 1;
    }
    if (st) {
        /* Out of order: the other buffer drained first. Resynchronize. */
        frames_played += dma_frames[next_drain];
        refill(next_drain);
    }
    notify_if_low();
}

static void hw_stop(void)
{
    /* wavedev stop (0x01911CF0). */
    AUD_ARM = 0;
    AUD_CTRL &= ~AUD_CTRL_OUT;
    AUD_POWER |= 1;
    AUD_DMA_RUN &= ~1u;
    AUD_START &= ~2u;
    codec_cmd(0xD007);
    AUD_ACK0 = 0x11;
    AUD_ACK1 = 0x11;
}

static int check(struct proc *p)
{
    if (!(p->flags & PROC_AUDIO))
        return GZ_EPERM;
    return present ? 0 : GZ_ENOENT;
}

int sys_audio_bind(struct proc *p, int port)
{
    int rv = check(p);

    if (rv)
        return rv;
    bound_port = port;
    notify_if_low();
    return 0;
}

int sys_audio_start(struct proc *p, uint32_t rate, uint32_t channels)
{
    uint32_t code, x2;
    int rv = check(p);

    if (rv)
        return rv;
    bool start_idle = channels & GZ_AUDIO_START_IDLE;
    channels &= ~GZ_AUDIO_START_IDLE;
    if (rate_setup(rate, &code, &x2) || (channels != 1 && channels != 2))
        return GZ_EINVAL;
    if (running)
        hw_stop();
    running = false;
    idle = start_idle;
    cur_rate = rate;
    cur_channels = channels;
    refill(0);
    refill(1);
    next_drain = 0;

    /* wavedev start (0x01911374): format and buffers, then enables. */
    AUD_FORMAT = (AUD_FORMAT & ~6u) | (channels == 1 ? 2u : 0u);
    AUD_START |= 2;
    AUD_ACK0 = 0x10;
    AUD_ACK1 = 0x10;
    AUD_ACK0 = 0x11;
    AUD_ACK1 = 0x11;
    AUD_POWER |= 1;
    AUD_CTRL = (AUD_CTRL & ~AUD_CTRL_RATE_MASK) | code | AUD_CTRL_OUT;
    AUD_GO = 1;
    codec_cmd(0x5000);
    codec_cmd((AUD_CODEC_STATUS & 0x80u) | 0xD007);
    codec_cmd(0xD007 | x2 << 7);
    codec_cmd(0xD001 | x2 << 7);
    AUD_DMA_RUN = 1;
    AUD_ARM = 1;
    running = true;
    return 0;
}

int sys_audio_write(struct proc *p, uint32_t ubuf, uint32_t len)
{
    uint8_t chunk[256];
    uint32_t done = 0;
    int rv = check(p);

    if (rv)
        return rv;
    /* Whole samples; the server keeps stereo frames together. */
    len = MIN(len, RING_BYTES - (ring_head - ring_tail)) & ~1u;
    while (done < len) {
        uint32_t n = MIN(len - done, sizeof(chunk));
        if (copy_from_as(&p->as, chunk, ubuf + done, n))
            return done ? (int)done : GZ_EINVAL;
        for (uint32_t k = 0; k < n; k++)
            ring[(ring_head + k) % RING_BYTES] = chunk[k];
        ring_head += n;
        done += n;
    }
    return (int)done;
}

int sys_audio_stop(struct proc *p, uint32_t flush)
{
    int rv = check(p);

    if (rv)
        return rv;
    if (running)
        hw_stop();
    running = false;
    if (flush) {
        ring_tail = ring_head;
        mix_tail = mix_head;
    }
    return 0;
}

int sys_audio_mix(struct proc *p, uint32_t ubuf, uint32_t len)
{
    uint8_t chunk[256];
    uint32_t done = 0;
    int rv = check(p);

    if (rv)
        return rv;
    /* Whole frames, so the mix stays aligned with the DMA buffers. */
    uint32_t frame = cur_channels == 2 ? 4 : 2;
    len = MIN(len, MIX_BYTES - (mix_head - mix_tail)) / frame * frame;
    while (done < len) {
        uint32_t n = MIN(len - done, sizeof(chunk));
        if (copy_from_as(&p->as, chunk, ubuf + done, n))
            return done ? (int)done : GZ_EINVAL;
        for (uint32_t k = 0; k < n; k++)
            mix[(mix_head + k) % MIX_BYTES] = chunk[k];
        mix_head += n;
        done += n;
    }
    return (int)done;
}

int sys_audio_status(struct proc *p, uint32_t uout)
{
    struct gz_audio_status st;
    int rv = check(p);

    if (rv)
        return rv;
    memset(&st, 0, sizeof(st));
    st.running = running;
    st.rate = cur_rate;
    st.channels = cur_channels;
    st.queued = ring_head - ring_tail;
    st.frames_played = frames_played;
    st.underruns = underruns;
    st.mix_queued = mix_head - mix_tail;
    return copy_to_as(&p->as, uout, &st, sizeof(st));
}
