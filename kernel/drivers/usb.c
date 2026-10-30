/* Casio BE-300 USB function controller. Polled, bounded, IRQ-independent.
 * Register evidence: be300/docs/BE300_USB_FUNCTION.md. RX uses PIO; TX DMA
 * uses an allocator-owned uncached page, never CE's old low-memory rings. */
#include "../kernel.h"
#ifndef USB_TEST
#include "../hw.h"
#endif
#include "../drivers.h"
#include <glaze/usb.h>

#ifndef USB_READ
#define USB_READ(off) REG32(0x0a000000u + (off))
#define USB_WRITE(off, v) (REG32(0x0a000000u + (off)) = (v))
#endif
#define R(off) USB_READ(off)
#define W(off, v) USB_WRITE(off, v)
#define RING 8192u
static uint8_t rx[RING], tx[RING], logs[RING];
static uint32_t rh, rt, th, tt, lh, lt, dropped;
static uint8_t *dma;
static uint32_t dma_len;
static bool network_debugger;
static bool active, configured, tx_busy, short_pending, zlp, status_pending;
static uint8_t lines, pending_lines;
static uint8_t diagnostic;
static uint32_t peek_addr;      /* diagnostics 3-5: KSEG1 address to read or write */
static uint32_t poke_value;     /* loaded by vendor request 0x5b */
static uint32_t generation;

void usb_log_putc(char c)
{
    if (lh - lt == RING) { lt++; dropped++; }
    logs[lh++ & (RING - 1)] = (uint8_t)c;
}
void usb_stop(void)
{
    usbnet_stop();
    if (!active) return;
    W(0x132c, 0); W(0x1488, 0); W(0x130c, 0x10);
    W(0x1308, 0);
    active = false;
}
void usb_init(void)
{
    /* The companion bit remains masked; timer polling also works while
     * developing exception/IRQ handling. */
    KJ_GIRQ0_EN(7) &= ~KJ_SRC_ENABLE;
    void *page = page_alloc();
    if (!page) return;
    mm_sync_code(page, PAGE_SIZE);
    dma = (uint8_t *)KSEG1(PHYS(page));
    W(0x1114, 1);
    for (volatile unsigned i = 0; i < 200000; i++) ;
    W(0x1300, 1); W(0x130c, 0x10); W(0x132c, 0); W(0x1488, 0);
    /* Match Casio's sequence: pulse reset with the unit enabled. */
    W(0x1300, 1); W(0x1308, 0);
    W(0x1338, 1); W(0x1338, 0); W(0x1444, 1);
    W(0x1494, 1); W(0x1494, ~1u);
    static const uint8_t descriptors[] = {
        18,1,0,1,255,255,255,16,0xcf,7,2,0x20,0,1,0,0,0,1,
        9,2,39,0,1,1,0,0x40,1,
        9,4,0,0,3,255,255,255,0,
        7,5,0x81,2,64,0,0, 7,5,2,2,64,0,0, 7,5,0x83,3,8,0,1
    };
    for (uint32_t i = 0; i < sizeof(descriptors); i++) W(0x153c + i * 4, descriptors[i]);
    W(0x1500, 1); W(0x1510, 0); W(0x1518, 0); W(0x1520, 0); W(0x1528, 0);
    W(0x148c, 0x3f); W(0x1470, 0x7d); W(0x1474, 0x38); W(0x1478, 9);
    W(0x14c0, 0x22); W(0x1308, 1); W(0x132c, 0x1f); W(0x1444, 0);
    active = true;
}
static void clear_session(void)
{
    usbnet_stop();
    if (!network_debugger) usb_input_cancel();
    rh = rt = th = tt = 0;
    tx_busy = short_pending = zlp = status_pending = false;
    diagnostic = 0;
    configured = false; lines = 0; generation++;
    W(0x130c, 0x10); W(0x1328, 3); W(0x148c, 0x3f);
}
void usb_poll(void)
{
    if (!active) return;
    uint32_t s1 = R(0x1468), s2 = R(0x146c);
    if (s2 & 2) { clear_session(); W(0x1484, ~2u); }
    if (s2 & 4) {
        bool now = (R(0x1450) & 0x80) != 0;
        if (!now) clear_session();
        configured = now; W(0x1484, ~4u);
    }
    if (s1 & 1) {
        uint8_t b[8];
        for (unsigned i = 0; i < 8; i++) { W(0x1480, ~0x10u); b[i] = R(0x14cc); }
        if (R(0x1468) & 0x10) return; /* overwritten: reread next poll */
        if (b[0] == 0x21 && b[1] == 0x22 && !b[3] && !b[4] && !b[5] && !b[6] && !b[7]) {
            pending_lines = b[2] & 3; status_pending = true;
        } else if (b[0] == 0x40 && b[1] == 0x5a && b[2] < 2 &&
                   !b[3] && !b[4] && !b[5] && !b[6] && !b[7]) {
            /* Out-of-band diagnostics stay usable if bulk DMA is broken.
             * The host drains EP3 before issuing this vendor request. */
            diagnostic = b[2] + 1; pending_lines = lines; status_pending = true;
        } else if (b[0] == 0x40 && b[1] == 0x5a && b[2] >= 2 && b[2] <= 4 &&
                   (b[3] >> 6) < 3 && !b[6] && !b[7] && hwdump_active()) {
            /* HWDUMP=1 bring-up only: read (2) or write the word loaded by
             * 0x5b, 32-bit (3) or 16-bit (4), in RAM, the companion or the
             * VR4131 (wValue high bits 7-6), word index in wValue bits 5-0
             * and wIndex. Lets a cold boot be examined and changed while
             * nothing else works; a gated companion unit still hangs the
             * bus, so the host must only touch live addresses. */
            static const uint32_t base[] = { 0xA0000000u, 0xAA000000u, 0xAF000000u };
            peek_addr = base[b[3] >> 6] + ((uint32_t)(b[3] & 0x3F) << 18 | b[5] << 10 | b[4] << 2);
            diagnostic = b[2] + 1; pending_lines = lines; status_pending = true;
        } else if (b[0] == 0x40 && b[1] == 0x5b && !b[6] && !b[7] && hwdump_active()) {
            poke_value = (uint32_t)b[2] | b[3] << 8 | b[4] << 16 | (uint32_t)b[5] << 24;
            pending_lines = lines; status_pending = true;
        } else { W(0x1460, R(0x1460) | 8); W(0x148c, 1); }
        W(0x1480, ~1u);
    }
    if (status_pending && (R(0x1468) & 0x20)) {
        W(0x148c, 2); W(0x1490, 1);
        W(0x147c, ~0x20u); W(0x1480, ~0x20u);
        if (lines && !pending_lines) {
            bool saved = configured; clear_session(); configured = saved;
        }
        lines = pending_lines; status_pending = false;
        if (!(R(0x1454) & 0x10)) {
            if (diagnostic) {
                uint8_t d[8] = {'G', 'Z'};
                if (diagnostic >= 3) {
                    if (diagnostic == 4)
                        *(volatile uint32_t *)(uintptr_t)peek_addr = poke_value;
                    else if (diagnostic == 5)
                        *(volatile uint16_t *)(uintptr_t)peek_addr = (uint16_t)poke_value;
                    uint32_t w = *(volatile uint32_t *)(uintptr_t)peek_addr;
                    d[2] = w; d[3] = w >> 8; d[4] = w >> 16; d[5] = w >> 24;
                    d[6] = diagnostic == 3 ? 'P' : 'W'; d[7] = generation;
                } else if (diagnostic == 1) {
                    d[2] = R(0x1324); d[3] = R(0x1464); d[4] = R(0x1468);
                    d[5] = R(0x146c); d[6] = R(0x1454); d[7] = R(0x14d8);
                } else {
                    d[2] = rh; d[3] = rt; d[4] = th; d[5] = tt;
                    d[6] = (configured ? 1 : 0) | (lines << 1) | (tx_busy ? 8 : 0) | (short_pending ? 16 : 0);
                    d[7] = generation;
                }
                for (unsigned i = 0; i < sizeof(d); i++) W(0x14e0, d[i]);
            } else {
                W(0x14e0, (lines & 1 ? 0xa0 : 0) | (lines & 2 ? 0x10 : 0)); W(0x14e0, 0);
            }
            W(0x1490, 4);
        }
        diagnostic = 0;
    }
    if (R(0x146c) & 8) W(0x1484, ~8u);
    if (!configured) return;
    /* At most four packets per poll: do not monopolize a timer interrupt. */
    for (unsigned k = 0; k < 4 && (R(0x1454) & 8); k++) {
        uint32_t n = R(0x14d8);
        if (n > 64 || n > RING - (rh - rt)) break;
        if (!n) { /* A zero-length OUT packet is consumed by a zero DMA. */
            if (tx_busy) break;
            W(0x1310, 0); W(0x1318, 0); W(0x131c, 0);
            W(0x1320, (uint32_t)PHYS(dma) | 0xa0000000u); W(0x130c, 1); W(0x1328, 1); break;
        }
        for (uint32_t i = 0; i < n; i++) rx[rh++ & (RING - 1)] = R(0x14d4);
    }
    if (tx_busy && (R(0x1324) & 2)) {
        W(0x1328, 2); tx_busy = false; tt += dma_len;
        if (dma_len < 64) { W(0x1490, 2); short_pending = true; }
        else if (tt == th) zlp = true;
    }
    if (short_pending) {
        if (!(R(0x1464) & 4)) return;
        short_pending = false;
    }
    if (tx_busy || !(R(0x1464) & 4)) return;
    if (tt == th) {
        W(0x1488, 0);
        if (zlp) { W(0x148c, 4); W(0x1490, 2); zlp = false; short_pending = true; }
        return;
    }
    zlp = false;
    dma_len = MIN(64u, th - tt);
    for (uint32_t i = 0; i < dma_len; i++) dma[i] = tx[(tt + i) & (RING - 1)];
#ifndef USB_TEST
    __asm__ volatile("sync" ::: "memory");
#endif
    W(0x1488, 0x85);
    W(0x1310, 1); W(0x1318, 0); W(0x131c, dma_len); W(0x1320, (uint32_t)PHYS(dma) | 0xa0000000u);
    W(0x1490, 0x10); W(0x130c, 1); tx_busy = true;
}
void usb_panic_poll(void)
{
    usb_poll();
    if (!active || !configured || !(lines & 1) || th - tt > RING - 300) return;
    uint8_t data[128], frame[GU_HEADER + sizeof(data)];
    uint32_t n = MIN(lh - lt, sizeof(data) - 4);
    if (!n && !dropped) return;
    gu_put32(data, dropped); dropped = 0;
    for (uint32_t i = 0; i < n; i++) data[4 + i] = logs[lt++ & (RING - 1)];
    uint32_t size = gu_encode(frame, GU_LOG, 0, 0, data, n + 4);
    for (uint32_t i = 0; i < size; i++) tx[th++ & (RING - 1)] = frame[i];
}
bool usb_tx_idle(void)
{
    usb_poll();
    return tt == th && !tx_busy && !short_pending && !zlp && (R(0x1464) & 4);
}
int sys_usb_ctl(struct proc *p, uint32_t op, uint32_t buf, uint32_t len)
{
    if (!(p->flags & PROC_USB)) return GZ_EPERM;
    if (op >= GZ_USB_NET_OPEN && op <= GZ_USB_NET_CLOSE) {
        usb_poll();
        if (op != GZ_USB_NET_CLOSE && (!configured || !(lines & 1))) return GZ_EDEAD;
        return usbnet_ctl(p, op, buf, len);
    }
    if ((op >= GZ_USB_RELOAD_BEGIN && op <= GZ_USB_RELOAD_ABORT) ||
        op == GZ_USB_RELOAD_DEBUG || op == GZ_USB_DEBUG_RESTORE) return usb_reload_ctl(p, op, buf, len);
    if (op == GZ_USB_DEBUG_TRANSPORT) {
        uint32_t value;
        if (len!=4 || copy_from_as(&p->as,&value,buf,4) || value>1) return GZ_EINVAL;
        network_debugger=value; return 0;
    }
    usb_poll();
    if (op == GZ_USB_STATE) return (int)((generation << 2) | (configured ? 1u : 0u) | (lines & 1 ? 2u : 0u));
    if (op == GZ_USB_LOG_READ) {
        uint8_t b[256];
        if (len < 8) return GZ_EINVAL;
        uint32_t n = MIN(MIN(len - 4, sizeof(b) - 4), lh - lt);
        gu_put32(b, dropped);
        for (uint32_t i = 0; i < n; i++) b[4 + i] = logs[(lt + i) & (RING - 1)];
        if (copy_to_as(&p->as, buf, b, n + 4)) return GZ_EINVAL;
        lt += n; dropped = 0; return n + 4;
    }
    if (op == GZ_USB_REBOOT_CE) {
        if (len) return GZ_EINVAL;
        uint64_t end = time_us() + 2000000u;
        while (!usb_tx_idle()) if (time_us() >= end) return GZ_ETIMEDOUT;
        machine_reboot_ce();
    }
    if ((op >= GZ_USB_SCRIPT_BEGIN && op <= GZ_USB_SCRIPT_CANCEL) || op == GZ_USB_LIVE_INPUT)
        return usb_input_ctl(p, op, buf, len);
    if (op == GZ_USB_SCREEN_COPY) {
        if (len != GU_SCREEN_BYTES || buf > 0x80000000u - len) return GZ_EINVAL;
#ifndef USB_FRAMEBUFFER
#define USB_FRAMEBUFFER ((const uint16_t *)KSEG1(FB_PA))
#endif
        /* Kernel execution is non-preemptive, so SpringBoard cannot flush
         * another frame while this snapshot is copied. Omit LCD row padding. */
        for (unsigned y = 0; y < GZ_FB_HEIGHT; y++)
            if (copy_to_as(&p->as, buf + y * GZ_FB_WIDTH * 2,
                           USB_FRAMEBUFFER + y * GZ_FB_STRIDE, GZ_FB_WIDTH * 2)) return GZ_EINVAL;
        return 0;
    }
    if (!configured || !(lines & 1)) return GZ_EDEAD;
    uint8_t b[256];
    uint32_t n = MIN(len, sizeof(b));
    if (op == GZ_USB_READ) {
        n = MIN(n, rh - rt);
        for (uint32_t i = 0; i < n; i++) b[i] = rx[(rt + i) & (RING - 1)];
        if (copy_to_as(&p->as, buf, b, n)) return GZ_EINVAL;
        rt += n; return n;
    }
    if (op == GZ_USB_WRITE) {
        n = MIN(n, RING - (th - tt));
        if (copy_from_as(&p->as, b, buf, n)) return GZ_EINVAL;
        for (uint32_t i = 0; i < n; i++) tx[th++ & (RING - 1)] = b[i];
        usb_poll(); return n;
    }
    return GZ_EINVAL;
}
