/*
 * Wave output: the BE-300's ping-pong PCM DMA block.
 *
 * This is a Casio block in the Kj companion window, not the VRC4173 AIU of
 * UM chapter 10. Its register map and start/stop sequences are reverse-
 * engineered from WinCE's wavedev.dll (be300 docs/BE300_AUDIO.md) and are
 * modelled by the emulator (be300 src/hw/aiu.c). The DMA plays buffer 0,
 * then buffer 1, and so on; completion flags in AUD_STATUS raise GIRQ0
 * bit 8 while the source enable at
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
 * The address pairs are current/next, not two immutable buffer slots:
 * hardware copies next into current as a buffer drains. wavedev's refill
 * helper at 0x01912174 writes the newly filled buffer to 0x8B8/0x8BC
 * regardless of its index. Reprogram both pairs on each playback start.
 * The exact enable/ack split across 0x8C4/0x8C8/0x8CC remains uncertain.
 *
 * Buffers are filled through KSEG1, so the uncached writes reach the DMA
 * without per-refill cache maintenance. The allocator's cached zeroing
 * must be written back and invalidated before first using those aliases.
 */

#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"

#define DMA_BYTES       PAGE_SIZE       /* per buffer; one page each */
#define RING_BYTES      GZ_AUDIO_RING
#define MIX_BYTES       GZ_AUDIO_MIX_RING
#define CODEC_TIMEOUT_US 10000u
#define DIAG_STARTS      4u

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
static uint32_t diag_starts, diag_drains;
static uint32_t master_gain = 32768;    /* Q15, set by the audio server */

static void hw_dump(const char *stage)
{
    kprintf("audio: %s ctrl=%x power=%x format=%x ready=%x codec=%x wake=%x/%x\n",
            stage, AUD_CTRL, AUD_POWER, AUD_FORMAT, AUD_CODEC_READY,
            AUD_CODEC_STATUS, AUD_WAKE_RUN, AUD_WAKE_CODEC);
    kprintf("audio: dma run=%x arm=%x status=%x source=%x buf=%x..%x/%x..%x\n",
            AUD_DMA_RUN, AUD_ARM, AUD_STATUS, KJ_SRC_EN,
            AUD_BUF0_START, AUD_BUF0_END, AUD_BUF1_START, AUD_BUF1_END);
}

static void delay_us(uint32_t us)
{
    uint64_t start = time_us();

    while (time_us() - start < us)
        ;
}

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

static int codec_wait(uint32_t cmd, const char *stage)
{
    uint64_t start = time_us();

    while (!AUD_CODEC_READY)
        if (time_us() - start >= CODEC_TIMEOUT_US) {
            kprintf("audio: codec timeout %s command=%x\n", stage, cmd);
            hw_dump("timeout");
            return GZ_ETIMEDOUT;
        }
    return 0;
}

static int codec_cmd(uint32_t cmd)
{
    int rv = codec_wait(cmd, "before");

    if (rv)
        return rv;
    AUD_CODEC_CMD = cmd;
    return codec_wait(cmd, "after");
}

/* Also used on timeout: do not send another command to a busy codec. */
static void hw_disable(void)
{
    KJ_SRC_EN &= ~AUD_SRC_EN_BIT;
    AUD_ARM = 0;
    AUD_CTRL16 &= ~AUD_CTRL_OUT;
    AUD_POWER16 |= 1;
    AUD_DMA_RUN &= ~1u;
    AUD_START &= ~2u;
}

static void hw_ack(void)
{
    AUD_ACK0_16 = 0x11;
    AUD_ACK1_16 = 0x11;
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
    /* Scale at the last stage so changing volume affects already queued
     * music and effects within two DMA buffers, without restarting them. */
    if (master_gain != 32768) {
        int16_t *samples = (int16_t *)d;
        for (uint32_t k = 0; k < DMA_BYTES / 2; k++)
            samples[k] = (int16_t)((int32_t)samples[k] * (int32_t)master_gain >> 15);
    }
}

static void program_buffers(void)
{
    AUD_BUF0_START = KSEG1(PHYS(dma_buf[0]));
    AUD_BUF0_END = KSEG1(PHYS(dma_buf[0])) + DMA_BYTES - 1;
    AUD_BUF1_START = KSEG1(PHYS(dma_buf[1]));
    AUD_BUF1_END = KSEG1(PHYS(dma_buf[1])) + DMA_BYTES - 1;
}

static void drained(uint32_t i)
{
    frames_played += dma_frames[i];
    refill(i);
    /* wavedev 0x01912174: replenish the NEXT pair, even for buffer 0.
     * Writing only RAM would leave the hardware replaying the old next
     * buffer and reporting the same completion bit over and over. */
    AUD_BUF1_START = KSEG1(PHYS(dma_buf[i]));
    AUD_BUF1_END = KSEG1(PHYS(dma_buf[i])) + DMA_BYTES - 1;
    next_drain = i ^ 1u;
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
    mm_sync_code(pages, 2 * DMA_BYTES);
    dma_buf[0] = (uint8_t *)KSEG1(PHYS(pages));
    dma_buf[1] = (uint8_t *)KSEG1(PHYS(pages) + DMA_BYTES);
    hw_dump("inherited");
    KJ_SRC_EN &= ~AUD_SRC_EN_BIT;
    hw_disable();
    hw_ack();
    /* wavedev's initialization (0x01912478), independent of WinCE's
     * last volume, playback and power state. */
    AUD_WAKE_CODEC = 1;
    AUD_CTRL16 |= AUD_CTRL_IDLE;
    AUD_FORMAT16 = 3;
    (void)codec_cmd(0xD007);  /* A failed codec can be retried on start. */
    program_buffers();
    /* Keep the source masked until playback starts. A codec/power event
     * with no buffer completion must not interrupt an idle driver. */
    present = true;
    hw_dump("initialized");
}

void audio_irq(void)
{
    uint32_t st;

    if (!running) {
        KJ_SRC_EN &= ~AUD_SRC_EN_BIT;
        return;
    }
    st = AUD_STATUS & 3u;
    if (!st)
        return;
    KJ_SRC_EN &= ~AUD_SRC_EN_BIT;
    if (diag_drains) {
        kprintf("audio: drained status=%x next=%u frames=%u queued=%u mix=%u\n",
                st, next_drain, frames_played, ring_head - ring_tail,
                mix_head - mix_tail);
        diag_drains--;
    }
    /* wavedev's IST (0x0191274C) acknowledges with 0x11 to both. */
    hw_ack();
    /* Queue completion flags are not RAM-buffer identities: real
     * hardware repeatedly reports status 1 during normal playback.
     * wavedev 0x019121A0 tracks/toggles the freed buffer in software.
     * Both flags pending account for two completions (also used by the
     * emulator's alternating completion-bit model). */
    uint32_t completed = (st & 1u) + ((st >> 1) & 1u);
    while (completed--)
        drained(next_drain);
    notify_if_low();
    /* If the hardware has not cleared the completion level, leave the
     * source masked. timer_irq also polls us, so a stuck source cannot
     * starve the scheduler or the UI. */
    if (!(AUD_STATUS & 3u))
        KJ_SRC_EN |= AUD_SRC_EN_BIT;
}

static int hw_stop(void)
{
    /* wavedev stop (0x01911CF0). */
    hw_disable();
    int rv = codec_cmd(0xD007);
    AUD_CTRL |= AUD_CTRL_IDLE;
    AUD_POWER &= ~AUD_POWER_CODEC;
    hw_ack();
    running = false;
    if (diag_starts && diag_starts <= DIAG_STARTS)
        hw_dump("stopped");
    return rv;
}

static int check(struct proc *p)
{
    if (!(p->flags & PROC_AUDIO))
        return GZ_EPERM;
    return present ? 0 : GZ_ENOENT;
}

int sys_audio_gain(struct proc *p, uint32_t gain)
{
    int rv = check(p);

    if (rv)
        return rv;
    if (gain > 32768)
        return GZ_EINVAL;
    master_gain = gain;
    return 0;
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
    if (running && (rv = hw_stop()))
        return rv;
    running = false;
    idle = start_idle;
    cur_rate = rate;
    cur_channels = channels;
    next_drain = 0;

    /* wavedev start (0x01911B8C, 0x01911388, 0x019111DC).
     * The emulator doesn't gate DMA on the codec power/clock bits, but
     * real hardware needs these writes and the final settling delay. */
    AUD_FORMAT16 = (AUD_FORMAT16 & ~2u) | (channels == 1 ? 2u : 0u);
    AUD_START |= 2;
    hw_ack();
    AUD_POWER16 |= 1;
    AUD_CTRL16 |= AUD_CTRL_OUT;
    if (!(AUD_POWER16 & 4u)) {
        AUD_ARM16 = 0;
        delay_us(1000);
    }
    AUD_WAKE_RUN = 1;
    AUD_GO = 1;
    AUD_ACK1_16 = 0x11;
    AUD_ACK0 = AUD_ACK1_16;
    AUD_CTRL = (AUD_CTRL16 & ~AUD_CTRL_RATE_MASK) | code;
    AUD_FORMAT &= ~4u;
    if ((rv = codec_cmd(0x5000)) ||
        (rv = codec_cmd((AUD_CODEC_STATUS & 0x80u) | 0xD007)) ||
        (rv = codec_cmd(0xD007 | x2 << 7)))
        goto failed;
    AUD_POWER |= AUD_POWER_CODEC;
    AUD_CTRL |= AUD_CTRL_IDLE;
    if ((rv = codec_cmd(0xD001 | x2 << 7)))
        goto failed;
    delay_us(5000);
    refill(0);
    refill(1);
    program_buffers();
    AUD_DMA_RUN = 1;
    AUD_ARM = 1;
    running = true;
    KJ_SRC_EN |= AUD_SRC_EN_BIT;
    if (diag_starts++ < DIAG_STARTS) {
        uint32_t nonzero = 0;
        for (uint32_t i = 0; i < DMA_BYTES; i++)
            nonzero += dma_buf[0][i] != 0 || dma_buf[1][i] != 0;
        kprintf("audio: start %u Hz %u ch nonzero=%u\n", rate, channels, nonzero);
        hw_dump("started");
        diag_drains = 2;
    }
    return 0;

failed:
    hw_disable();
    AUD_POWER &= ~AUD_POWER_CODEC;
    hw_ack();
    return rv;
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
        rv = hw_stop();
    running = false;
    if (flush) {
        ring_tail = ring_head;
        mix_tail = mix_head;
    }
    return rv;
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
