/* Production audio driver against fake MMIO and a controllable codec.
 * The emulator starts DMA without checking power or codec readiness, so
 * hardware initialization and timeout regressions need a separate check. */
#include "../kernel/kernel.h"
#include "../kernel/drivers.h"
#include "../kernel/hw.h"
#include <assert.h>
#include <stdio.h>

static uint32_t regs[0x1200 / 4];
static uint8_t pages[2 * PAGE_SIZE], user_pcm[2 * PAGE_SIZE];
static uint64_t now, ready_at, dma_started_at;
static bool blocked, dma_seen, pages_synced;
static unsigned command_count, fail_command;
static struct {
    uint32_t value, power, ctrl;
} commands[32];

static void observe_dma(void)
{
    if (!dma_seen && (regs[0x898 / 4] & 1) && (regs[0x8a0 / 4] & 1)) {
        dma_started_at = now;
        dma_seen = true;
    }
}

static volatile uint32_t *mmio(uint32_t pa)
{
    unsigned off = pa - 0x0A000000u;
    assert(off < sizeof(regs));
    observe_dma();
    if (off == 0x3c4) {
        uint32_t cmd = regs[0x3c0 / 4];
        if (cmd) {
            assert(command_count < ARRAY_SIZE(commands));
            commands[command_count].value = cmd;
            commands[command_count].power = regs[0x884 / 4];
            commands[command_count].ctrl = regs[0x880 / 4];
            command_count++;
            regs[0x3c0 / 4] = 0;  /* command consumed by the fake codec */
            ready_at = now + 300;
            if (command_count == fail_command)
                blocked = true;
        }
        regs[off / 4] = !blocked && now >= ready_at;
    }
    return &regs[off / 4];
}

#undef REG32
#undef REG16
#undef KSEG1
#undef PHYS
#define REG32(pa) (*mmio(pa))
#define REG16(pa) (*(volatile uint16_t *)mmio(pa))
#define KSEG1(pa) ((uintptr_t)(pa))
#define PHYS(va) ((uintptr_t)(va))
#include "../kernel/drivers/audio.c"

uint64_t time_us(void) { observe_dma(); now += 100; return now; }
void *page_alloc_n(uint32_t n) { assert(n == 2); return pages; }
void mm_sync_code(void *p, uint32_t n)
{
    assert(p == pages && n == sizeof(pages));
    pages_synced = true;
}
void kputs(const char *s) { (void)s; }
void kprintf(const char *fmt, ...) { (void)fmt; }
int ipc_notify(int p, uint32_t bits) { (void)p; (void)bits; return 0; }
int copy_from_as(struct addrspace *as, void *dst, uint32_t src, uint32_t n)
{
    (void)as;
    assert(src <= sizeof(user_pcm) && n <= sizeof(user_pcm) - src);
    memcpy(dst, user_pcm + src, n);
    return 0;
}
int copy_to_as(struct addrspace *as, uint32_t dst, const void *src, uint32_t n)
{
    (void)as; (void)dst; (void)src; (void)n;
    return 0;
}

static struct proc owner;

static void reset(void)
{
    memset(regs, 0, sizeof(regs));
    memset(pages, 0, sizeof(pages));
    memset(user_pcm, 0x23, sizeof(user_pcm));
    memset(&owner, 0, sizeof(owner));
    owner.flags = PROC_AUDIO;
    now = ready_at = dma_started_at = 0;
    blocked = dma_seen = pages_synced = false;
    fail_command = command_count = 0;
    running = present = idle = false;
    ring_head = ring_tail = mix_head = mix_tail = 0;
    frames_played = underruns = diag_starts = diag_drains = 0;
    master_gain = 32768;
    bound_port = -1;
    /* Model a handoff with playback still armed; init must stop it. */
    regs[0x880 / 4] = AUD_CTRL_OUT;
    regs[0x898 / 4] = regs[0x8a0 / 4] = 1;
    audio_init();
    assert(pages_synced);
    assert(present && !running && !AUD_DMA_RUN && !AUD_ARM);
    assert(AUD_WAKE_CODEC == 1 && (AUD_CTRL & AUD_CTRL_IDLE));
    assert(AUD_FORMAT == 3 && command_count == 1);
    assert(commands[0].value == 0xd007);
    assert(!(KJ_SRC_EN & AUD_SRC_EN_BIT));
    command_count = 0;
    dma_seen = false;
}

int main(void)
{
    reset();
    assert(sys_audio_write(&owner, 0, sizeof(user_pcm)) == sizeof(user_pcm));
    assert(sys_audio_start(&owner, 44100, 2) == 0);
    assert(command_count == 4);
    assert(commands[0].value == 0x5000 && commands[1].value == 0xd007);
    assert(commands[2].value == 0xd087 && commands[3].value == 0xd081);
    assert(commands[3].power & AUD_POWER_CODEC);
    assert(commands[3].ctrl & AUD_CTRL_IDLE);
    assert(AUD_WAKE_RUN == 1 && AUD_WAKE_CODEC == 1);
    assert(running && AUD_DMA_RUN == 1 && AUD_ARM == 1);
    assert(KJ_SRC_EN & AUD_SRC_EN_BIT);
    assert(!(AUD_FORMAT & 6) && (AUD_CTRL & AUD_CTRL_OUT));
    assert(dma_seen && dma_started_at >= ready_at + 5000);
    assert(!memcmp(pages, user_pcm, sizeof(pages)));
    assert(AUD_BUF0_END - AUD_BUF0_START + 1 == PAGE_SIZE);
    assert(AUD_BUF1_END - AUD_BUF1_START + 1 == PAGE_SIZE);
    /* Real hardware promotes NEXT into CURRENT on completion. Each
     * refill must enqueue the freed buffer, including buffer 0. */
    uint32_t address[2] = {(uint32_t)(uintptr_t)pages,
                          (uint32_t)(uintptr_t)(pages + PAGE_SIZE)};
    regs[0x8b0 / 4] = regs[0x8b8 / 4];
    regs[0x8b4 / 4] = regs[0x8bc / 4];
    regs[0x8a8 / 4] = 1;
    audio_irq();
    assert(frames_played == PAGE_SIZE / 4);
    assert(AUD_BUF0_START == address[1] && AUD_BUF1_START == address[0]);
    /* This fake does not clear status on ack: leave an uncleared level
     * masked instead of repeatedly taking the shared GIU interrupt. */
    assert(!(KJ_SRC_EN & AUD_SRC_EN_BIT));
    regs[0x8b0 / 4] = regs[0x8b8 / 4];
    regs[0x8b4 / 4] = regs[0x8bc / 4];
    regs[0x8a8 / 4] = 1; /* real hardware reports this for both RAM buffers */
    audio_irq();
    assert(frames_played == 2 * PAGE_SIZE / 4 && next_drain == 0);
    assert(AUD_BUF0_START == address[0] && AUD_BUF1_START == address[1]);

    /* Status 2 likewise does not choose RAM buffer 1: software rotation
     * says buffer 0 is next to be refilled here. */
    assert(sys_audio_write(&owner, 0, PAGE_SIZE) == PAGE_SIZE);
    regs[0x8a8 / 4] = 2;
    audio_irq();
    assert(!memcmp(pages, user_pcm, PAGE_SIZE));
    assert(pages[PAGE_SIZE] == 0 && AUD_BUF1_START == address[0] && next_drain == 1);
    assert(sys_audio_write(&owner, 0, sizeof(user_pcm)) == sizeof(user_pcm));
    regs[0x8a8 / 4] = 3;
    audio_irq();
    assert(!memcmp(pages, user_pcm, sizeof(pages)) && next_drain == 1);
    regs[0x8a8 / 4] = 0;
    assert(sys_audio_stop(&owner, 1) == 0);

    /* Master gain is applied once after mixing, including data queued
     * before the change. Apps cannot bypass the audio server to set it. */
    reset();
    struct proc client = {0};
    assert(sys_audio_gain(&client, 0) == GZ_EPERM);
    assert(sys_audio_gain(&owner, 32769) == GZ_EINVAL);
    int16_t *input = (int16_t *)user_pcm;
    input[0] = 12000; input[1] = -12000;
    assert(sys_audio_write(&owner, 0, 4) == 4);
    input[0] = 4000; input[1] = -4000;
    assert(sys_audio_mix(&owner, 0, 4) == 4);
    assert(sys_audio_gain(&owner, 16384) == 0);
    assert(sys_audio_start(&owner, 22050, 1) == 0);
    int16_t *output = (int16_t *)pages;
    assert(output[0] == 8000 && output[1] == -8000);
    assert(sys_audio_write(&owner, 0, 4) == 4);
    assert(sys_audio_gain(&owner, 0) == 0);
    regs[0x8a8 / 4] = 1;
    audio_irq();
    assert(output[0] == 0 && output[1] == 0 && running);
    regs[0x8a8 / 4] = 0;
    assert(sys_audio_gain(&owner, 32768) == 0);
    assert(sys_audio_write(&owner, 0, 4) == 4);
    regs[0x8a8 / 4] = 2;
    audio_irq();
    output = (int16_t *)(pages + PAGE_SIZE);
    assert(output[0] == 4000 && output[1] == -4000 && running);
    regs[0x8a8 / 4] = 0;
    assert(sys_audio_stop(&owner, 1) == 0);
    assert(!running && !AUD_ARM && !AUD_DMA_RUN);
    assert(!(KJ_SRC_EN & AUD_SRC_EN_BIT));
    KJ_SRC_EN |= AUD_SRC_EN_BIT;
    audio_irq();
    assert(!(KJ_SRC_EN & AUD_SRC_EN_BIT));
    assert(!(AUD_POWER & AUD_POWER_CODEC) && !(AUD_CTRL & AUD_CTRL_OUT));
    /* Leave both live pairs at buffer 1, as seen on hardware after stop.
     * A restart must restore two distinct addresses before arming DMA. */
    regs[0x8b0 / 4] = regs[0x8b8 / 4];
    regs[0x8b4 / 4] = regs[0x8bc / 4];
    assert(sys_audio_start(&owner, 22050, 1) == 0);
    assert(AUD_FORMAT & 2);
    assert(AUD_BUF0_START == address[0] && AUD_BUF1_START == address[1]);
    assert(sys_audio_stop(&owner, 1) == 0);

    /* Busy before submission: no codec command or PCM may be consumed. */
    reset();
    assert(sys_audio_write(&owner, 0, sizeof(user_pcm)) == sizeof(user_pcm));
    blocked = true;
    uint64_t start = now;
    assert(sys_audio_start(&owner, 22050, 1) == GZ_ETIMEDOUT);
    assert(command_count == 0 && now - start < 13000);
    assert(!running && !AUD_ARM && !AUD_DMA_RUN && ring_tail == 0);

    /* Timeout after each command, including the final codec-on command. */
    for (unsigned cmd = 1; cmd <= 4; cmd++) {
        reset();
        fail_command = cmd;
        assert(sys_audio_start(&owner, 44100, 2) == GZ_ETIMEDOUT);
        assert(command_count == cmd);
        assert(!running && !AUD_ARM && !AUD_DMA_RUN);
        assert(!(AUD_POWER & AUD_POWER_CODEC));
        blocked = false;
        fail_command = 0;
        assert(sys_audio_start(&owner, 44100, 2) == 0);
        assert(sys_audio_stop(&owner, 1) == 0);
    }

    reset();
    assert(sys_audio_start(&owner, 22050, 1) == 0);
    blocked = true;
    assert(sys_audio_stop(&owner, 1) == GZ_ETIMEDOUT);
    assert(!running && !AUD_ARM && !AUD_DMA_RUN);

    /* A codec unavailable at boot must not prevent a later retry. */
    reset();
    blocked = true;
    present = false;
    audio_init();
    assert(present && !running && !AUD_ARM && !AUD_DMA_RUN);
    blocked = false;
    assert(sys_audio_start(&owner, 22050, 1) == 0);
    assert(sys_audio_stop(&owner, 1) == 0);
    puts("audio driver: PASS codec power/settle, DMA queue/refill/restart, IRQ masking and timeouts");
    return 0;
}
