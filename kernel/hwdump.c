/*
 * Hardware register snapshots for cold-boot bring-up (HWDUMP=1 builds).
 *
 * A NAND or CF-recovery boot starts from the masked ROM's state, while a
 * GlazeBoot RAM boot inherits Windows CE's. Comparing the same register
 * windows in both, at kernel entry and after driver setup, lists what CE
 * initializes and Glaze does not (docs/NAND_BOOT_AUDIT.md, tools/hwdump_diff.py).
 *
 * The windows follow what BEDiag already read safely on a running BE-300
 * (be300 docs/hardware/hw_dump_vr4131.txt, hw_dump_vrc4173.txt). Registers
 * that pop a FIFO, stream data or clear on read are left out; each
 * exclusion is noted in the table. Only kernel/main.c owns a buffer, under
 * GLAZE_HWDUMP, so ordinary builds capture nothing.
 *
 * Format (little-endian words): header { 'GZHW', version, slots, boot },
 * then per slot { 'SLOT', index, CP0 Status, CP0 Config, CP0 Count,
 * windows } and per window { pa, width << 16 | count, count values }; a
 * window skipped because its gate may be off has bit 31 set and no values.
 * boot is 0 for an SPL (NAND) boot, else 0x80000000 | gz_bootinfo flags.
 */

#include "kernel.h"
#include "hw.h"
#include "mips.h"
#include "proc.h"
#include "drivers.h"
#include <glaze/bootinfo.h>

#define HWDUMP_MAGIC    0x57485A47u     /* "GZHW" */
#define HWDUMP_SLOT     0x544F4C53u     /* "SLOT" */
#define HWDUMP_VERSION  1u

struct hw_window {
    uint32_t pa;
    uint8_t width;      /* access width in bytes */
    uint8_t stride;     /* bytes between registers */
    uint16_t count;
    uint8_t when;
};

/*
 * Reading a companion unit whose clock gate is off can stall the bus for
 * good on a real BE-300: a cold CF-recovery boot hung with a black screen
 * in an earlier version of this table, while CE (every gate on) and the
 * emulator (no gating) were fine. So kernel entry reads only what the ROM
 * has just used (NAND_ROM_HANDOFF.md, "Initialization supplied by the
 * ROM"), and the second snapshot, after Glaze's drivers have opened their
 * own gates, skips units whose gate is unknown while any gate Glaze never
 * sets (UNKNOWN_GATES) is still off.
 */
#define AT_ENTRY    1   /* live at the ROM, SPL or CE handoff */
#define AT_SETUP    2   /* after Glaze's drivers have enabled the unit */
#define IF_GATED    4   /* gate owner unknown: only with UNKNOWN_GATES all on */

static const struct hw_window windows[] = {
    /* VR4131 internal units, 16-bit registers (VR4131 UM ch. 7-13). */
    { 0x0F000000u, 2, 2, 32, AT_ENTRY | AT_SETUP },     /* BCU and DMAAU/DCU */
    { 0x0F000060u, 2, 2, 16, AT_ENTRY | AT_SETUP },     /* CMU: CMUCLKMSK unit clock masks */
    { 0x0F000080u, 2, 2, 32, AT_ENTRY | AT_SETUP },     /* ICU */
    { 0x0F0000C0u, 2, 2, 64, AT_ENTRY | AT_SETUP },     /* PMU, RTC, DSU */
    { 0x0F000140u, 2, 2, 48, AT_ENTRY | AT_SETUP },     /* GIU: direction, data, interrupt type */
    /* Casio companion ("VRC4173" in this tree), 32-bit words. */
    { 0x0A000000u, 4, 4, 64, AT_ENTRY | AT_SETUP },     /* core: GIRQ0 status and source enables */
    { 0x0A001000u, 4, 4, 4, AT_ENTRY | AT_SETUP },      /* socket card status (the ROM reads the card) */
    { 0x0A001100u, 4, 4, 16, AT_ENTRY | AT_SETUP },     /* clock/power gates (mirrored every 0x40) */
    { 0x0A001B20u, 4, 4, 4, AT_ENTRY | AT_SETUP },      /* the ROM writes 1B24/1B20 */
    { 0x0A007810u, 4, 4, 2, AT_ENTRY | AT_SETUP },      /* CMCU config, watchdog timeout */
    { 0x0A007834u, 4, 4, 1, AT_ENTRY | AT_SETUP },      /* CMCU status (the ROM polls it); commands left alone */
    { 0x0A00A000u, 4, 4, 15, AT_ENTRY | AT_SETUP },     /* socket bridge and timing; A03C acks the CF cause */
    { 0x0A00A400u, 4, 4, 8, AT_ENTRY | AT_SETUP },      /* NAND transfer enables, control, command */
    { 0x0A00A440u, 4, 4, 8, AT_ENTRY | AT_SETUP },      /* NAND status; A420 data, A430 ACK left alone */
    { 0x0A00A464u, 4, 4, 3, AT_ENTRY | AT_SETUP },      /* NAND mode; A460 is the kick */
    /* Units Glaze's own drivers read on a cold boot that reached SpringBoard. */
    { 0x0A0003C4u, 4, 4, 15, AT_SETUP },                /* codec ready..status; 3C0 is the command port */
    { 0x0A000880u, 4, 4, 20, AT_SETUP },                /* audio DMA block */
    { 0x0A000980u, 4, 4, 12, AT_SETUP },                /* BLG: backlight and buzzer PWM */
    { 0x0A000C00u, 4, 4, 24, AT_SETUP },                /* LCD controller */
    { 0x0A001300u, 4, 4, 16, AT_SETUP },                /* USB DMA/top level (gate 1114) */
    { 0x0A001440u, 4, 4, 33, AT_SETUP },                /* USB core; 14C4..14CC are FIFO ports */
    { 0x0A008684u, 1, 4, 1, AT_SETUP },                 /* SIU IER; RBR (+0) and IIR (+8) clear on read */
    { 0x0A00868Cu, 1, 4, 5, AT_SETUP },                 /* SIU LCR, MCR, LSR, MSR, SCR */
    { 0x0A00A044u, 4, 4, 1, AT_SETUP },                 /* key state; A040 is the key latch */
    { 0x0A00A048u, 4, 4, 110, AT_SETUP },               /* board control through A1FF (A0C4/A0C8 are read only here) */
    /* Gate owner unknown. */
    { 0x0A000200u, 4, 4, 16, AT_SETUP | IF_GATED },     /* unidentified unit, non-zero under CE */
    { 0x0A000300u, 4, 4, 48, AT_SETUP | IF_GATED },     /* PIU control and page buffers */
};

/* Gates Windows CE leaves at 1 that no Glaze driver writes. */
static const uint16_t unknown_gates[] = { 0x1108, 0x112C, 0x1138, 0x113C };

static uint32_t *buf;
static uint32_t buf_words, used;

static void put(uint32_t v)
{
    if (used < buf_words)
        buf[used] = v;
    used++;
}

static uint32_t rd(uint32_t pa, uint8_t width)
{
    if (width == 1) return REG8(pa);
    if (width == 2) return REG16(pa);
    return REG32(pa);
}

/* Only kernel/main.c's HWDUMP=1 build sets a buffer; usb.c's register
 * peek is limited to such builds. */
bool hwdump_active(void)
{
    return buf != NULL;
}

void hwdump_setup(uint32_t *words, uint32_t nwords)
{
    buf = words;
    buf_words = nwords;
    used = 0;
    put(HWDUMP_MAGIC);
    put(HWDUMP_VERSION);
    put(0);
    put(0);
}

static bool unknown_gates_on(void)
{
    for (unsigned i = 0; i < sizeof(unknown_gates) / sizeof(unknown_gates[0]); i++)
        if (!(REG32(0x0A000000u + unknown_gates[i]) & 1))
            return false;
    return true;
}

/* Boot-trace boxes 24 onwards (glaze/bootinfo.h): yellow while a window is
 * read, green once done, so a hang names the window on screen. */
static void mark(bool trace, unsigned i, uint16_t color)
{
    if (trace)
        gz_boot_trace(24 + i, color);
}

void hwdump_capture(uint32_t slot)
{
    if (!buf)
        return;
    uint8_t when = slot ? AT_SETUP : AT_ENTRY;
    bool trace = slot && ram_boot, gated = unknown_gates_on();
    put(HWDUMP_SLOT);
    put(slot);
    put(read_c0_status());
    put(read_c0_config());
    put(read_c0_count());
    for (unsigned i = 0; i < sizeof(windows) / sizeof(windows[0]); i++) {
        const struct hw_window *w = &windows[i];
        if (!(w->when & when))
            continue;
        if ((w->when & IF_GATED) && !gated) {
            put(w->pa);
            put(0x80000000u | (uint32_t)w->width << 16);    /* skipped */
            continue;
        }
        mark(trace, i, GZ_TRACE_YELLOW);
        put(w->pa);
        put((uint32_t)w->width << 16 | w->count);
        for (unsigned j = 0; j < w->count; j++)
            put(rd(w->pa + j * w->stride, w->width));
        mark(trace, i, GZ_TRACE_GREEN);
    }
    if (slot) {
        /* The SIU divisor latch sits behind LCR.DLAB (ns16550 §8.10). Its
         * LCR write is the only store here, restored at once. */
        uint8_t lcr = UART_LCR;
        UART_LCR = lcr | 0x80u;
        uint32_t dll = UART_RBR, dlm = REG8(UART_BASE + 1 * 4);
        UART_LCR = lcr;
        put(UART_BASE);
        put(1u << 16 | 2);
        put(dll);
        put(dlm);
    }
    buf[2] = slot + 1;
    if (ram_boot)
        buf[3] = 0x80000000u | ram_boot->flags;
}

int sys_hwdump(struct proc *p, uint32_t op, uint32_t uaddr, uint32_t len)
{
    if (!(p->flags & PROC_PRIVILEGED))
        return GZ_EPERM;
    if (!buf)
        return GZ_ENOENT;
    if (op == 1) {
        uint32_t cf[8];
        cf_debug(cf);
        if (len > sizeof(cf))
            len = sizeof(cf);
        return copy_to_as(&p->as, uaddr, cf, len) ? GZ_EINVAL : (int)len;
    }
    if (op)
        return GZ_EINVAL;
    uint32_t bytes = (used < buf_words ? used : buf_words) * 4;
    if (!uaddr)
        return (int)bytes;
    if (len > bytes)
        len = bytes;
    if (copy_to_as(&p->as, uaddr, buf, len))
        return GZ_EINVAL;
    return (int)len;
}
