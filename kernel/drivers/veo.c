/*
 * Veo Photo Traveler CF camera: bring-up probe (docs/hardware/VEO.md).
 *
 * Register interface, recovered from Veo's Windows CE veocam.dll: in the
 * card's 16-bit I/O window at B, a write of (reg << 8) | value to B+6
 * sets a bridge register; writing reg << 8 to B+4 and reading B+4 back
 * returns its value in the low byte; B+0 is the FIFO of the card's own
 * JPEG encoder, two bytes per read, high byte first, 0xFFFF when empty.
 * The camera sensor sits behind the bridge's two-wire sequencer.
 *
 * SYS_CAM_PROBE (PROC_USB) exposes this one step at a time for USB
 * diagnostics; the capture runs as a state machine, each poll doing a
 * bounded amount of FIFO work, because kernel code runs with interrupts
 * disabled. Built only with VEO_PROBE=1.
 */
#include "../kernel.h"
#include "../drivers.h"
#include "../proc.h"
#include "../hw.h"
#include <glaze/veo.h>
#include <string.h>

#ifndef GLAZE_VEO_PROBE
int sys_cam_probe(struct proc *p, uint32_t op, uint32_t arg, uint32_t ubuf, uint32_t len)
{
    (void)p; (void)op; (void)arg; (void)ubuf; (void)len;
    return GZ_EINVAL;   /* not a VEO_PROBE=1 kernel */
}
#else

#ifndef VEO_IO_WINDOW
#define VEO_IO_WINDOW 0x0A00C000u       /* PCMCIA I/O space (hw.h NE_BASE, CF_ATA_BASE) */
#endif
#ifndef VEO_IO_R16
#define VEO_ATTR8(off)      REG8(0x0A00D000u + (off))
#define VEO_IO_R16(pa)      REG16(pa)
#define VEO_IO_W16(pa, v)   (REG16(pa) = (v))
#define VEO_IO_R8(pa)       REG8(pa)
#define VEO_IO_W8(pa, v)    (REG8(pa) = (v))
#define VEO_DELAY_US(us)    veo_delay_us(us)
static void veo_delay_us(uint32_t us)
{
    uint64_t t0 = time_us();
    while (time_us() - t0 < us)
        ;
}
#endif

#define VEO_FIFO    0
#define VEO_READ    4
#define VEO_WRITE   6

/* Bridge registers (names describe the CE driver's use of them). */
#define R_CONTROL       0x0C    /* 0x80 arms the encoder, 0x00 stops it */
#define R_STATUS        0x0D    /* 0x80 bus busy, 0x40 abort, 0x20 data */
#define R_BUS_OPS_HI    0x1F    /* sequencer opcodes 15..8, bit 7 starts it */
#define R_BUS_OPS_LO    0x20
#define R_BUS_RESULT    0x26
#define R_PIPELINE      0xB3    /* 0x00 then 0x03 restarts the pipeline */
#define R_OUTPUT        0xE3    /* 0x4F preview, 0xFF still */
#define ST_BUSY         0x80
#define ST_ABORT        0x40
#define ST_DATA         0x20

#define SENSOR_WRITE    0xAA    /* the sensor's bus addresses */
#define SENSOR_READ     0xAB
#define SENSOR_ID       0x18    /* sensor register 0 */

static uint32_t io_base = 0x300;
static uint32_t base_pa = VEO_IO_WINDOW + 0x300;

static void wr(uint8_t reg, uint8_t value)
{
    VEO_IO_W16(base_pa + VEO_WRITE, (uint16_t)(reg << 8 | value));
}

static uint8_t rd(uint8_t reg)
{
    VEO_IO_W16(base_pa + VEO_READ, (uint16_t)(reg << 8));
    return (uint8_t)VEO_IO_R16(base_pa + VEO_READ);
}

static uint16_t fifo(void) { return VEO_IO_R16(base_pa + VEO_FIFO); }

/* ---- the bridge's two-wire sequencer ------------------------------------
 * Up to five 3-bit opcodes per run, packed low first into 0x20 (bits 7..0)
 * and 0x1F (bits 14..8, bit 15 = go); unused steps are filled with 7. A
 * step carrying a byte loads it into its slot's data register first. */
enum { OP_BYTE0 = 0, OP_START = 1, OP_STOP = 2, OP_SEND = 3, OP_ACK = 4,
       OP_RECV = 6, OP_PAD = 7, OP_RESULT = 8 };
static const uint8_t slot_reg[5] = {0x27, 0x2D, 0x2E, 0x2F, 0x30};

struct bus { uint16_t ops; unsigned n; uint8_t *result; bool timeout; };
static uint32_t bus_delay_us;

static void bus_run(struct bus *b)
{
    uint16_t ops = (uint16_t)(b->ops | (0xFFFFu << (3 * b->n)));

    wr(R_BUS_OPS_HI, 0);
    wr(R_BUS_OPS_LO, (uint8_t)ops);
    wr(R_BUS_OPS_HI, (uint8_t)(ops >> 8));
    uint64_t t0 = time_us();
    while (rd(R_STATUS) & ST_BUSY)
        if (time_us() - t0 > 20000u) { b->timeout = true; break; }
    if (bus_delay_us)
        VEO_DELAY_US(bus_delay_us);
    if (b->result) { *b->result = rd(R_BUS_RESULT); b->result = NULL; }
    b->ops = 0; b->n = 0;
}

static void bus_step(struct bus *b, unsigned op, uint8_t byte, uint8_t *result)
{
    unsigned code = op & 7;

    if (b->n > 4 || (result && b->result))
        bus_run(b);
    if (code == OP_BYTE0 || code == OP_SEND || code == OP_ACK)
        wr(slot_reg[b->n], byte);
    b->ops |= (uint16_t)(code << (3 * b->n));
    b->n++;
    if (result) b->result = result;
}

static void bus_address(struct bus *b, uint8_t reg)
{
    bus_step(b, OP_START, 0, NULL);
    bus_step(b, OP_SEND, SENSOR_WRITE, NULL);
    bus_step(b, OP_ACK, 0xFF, NULL);
    bus_step(b, OP_SEND, (uint8_t)(reg << 1), NULL);
    bus_step(b, OP_ACK, 0xFF, NULL);
}

static bool sensor_write(uint8_t reg, uint8_t value)
{
    struct bus b = {0};
    bus_address(&b, reg);
    bus_step(&b, OP_SEND, value, NULL);
    bus_step(&b, OP_ACK, 0xFF, NULL);
    bus_step(&b, OP_STOP, 0, NULL);
    bus_run(&b);
    return !b.timeout;
}

static int sensor_read(uint8_t reg)
{
    struct bus b = {0};
    uint8_t value = 0;
    bus_address(&b, reg);
    bus_step(&b, OP_START, 0, NULL);
    bus_step(&b, OP_SEND, SENSOR_READ, NULL);
    bus_step(&b, OP_ACK, 0xFF, NULL);
    bus_step(&b, OP_RECV | OP_RESULT, 0, &value);
    bus_step(&b, OP_ACK, 0xFF, NULL);
    bus_step(&b, OP_STOP, 0, NULL);
    bus_run(&b);
    return b.timeout ? GZ_ETIMEDOUT : value;
}

/* ---- bring-up and modes ------------------------------------------------- */

static void write_list(const uint16_t *list, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        VEO_IO_W16(base_pa + VEO_WRITE, list[i]);
}

/* Bridge reset, clocks and sensor power (the CE driver's divider 2). */
static int bring_up(bool still)
{
    static const uint16_t reset[] = {
        0x00C0, 0x0080, 0xB300, 0x1100, 0x1189, 0x2CBC, 0x2A01, 0x2BD8, 0x0F41,
    };
    static const uint16_t after[] = { 0xB008, 0xB138, 0xB22C, 0xB303, 0xE109 };

    write_list(reset, sizeof(reset) / sizeof(reset[0]));
    VEO_DELAY_US(30000);
    wr(0x0F, 0x61);
    wr(0x19, 0xB1);
    wr(0x08, 0x00);
    uint8_t r19 = rd(0x19);
    wr(0x19, still ? (r19 | 0x04) : (r19 & ~0x04));
    write_list(after, sizeof(after) / sizeof(after[0]));
    int id = sensor_read(0);
    if (id == SENSOR_ID)
        sensor_write(0x1C, 0);
    return id;
}

/* Mode tables: bridge registers 0x90-0x93 are the output size, 0x94-0x97
 * the sensor window (4 pixels wider and taller at full size), 0x98-0x99
 * fixed; 0x03, 0x05 and 0xB1 differ per mode. */
struct mode { uint16_t width, height, window_w, window_h; uint8_t r03, r05, rb1; };
static const struct mode modes[4] = {
    [1] = {640, 480, 644, 484, 0x84, 0xE4, 0x30},
    [2] = {320, 240, 640, 480, 0x80, 0xE0, 0x38},
    [3] = {160, 120, 640, 480, 0x82, 0xE2, 0x3B},
};

/* Per-channel gains the CE driver loads into 0x45-0x55 and 0x56-0x66. */
static const uint8_t gains[17] = {
    0x00, 0x9C, 0x01, 0xC0, 0x02, 0xC0, 0x4B, 0xAA, 0xDF, 0x7B, 0x53, 0x37, 0x1A,
    0x08, 0x08, 0x08, 0x08,
};

static void program_mode(unsigned m, bool fine)
{
    static const uint16_t colour[] = {
        0x9A00, 0x9B11, 0x9C00, 0x9D34, 0x9E01, 0x9FFB, 0xA001, 0xA1EB, 0xA201,
        0xA3DB, 0xA400, 0xA53D, 0xA600, 0xA73D, 0xA801, 0xA9CD, 0xAA01, 0xABF6,
    };
    static const uint16_t windows[] = {
        0x6701, 0x6800, 0x6978, 0x6A01, 0x6B90, 0x6C00, 0x6D32, 0x6E01, 0x6F90,
        0x7800, 0x79D2, 0x7A00, 0x7BD2, 0x7C00, 0x7DA0, 0x7E00, 0x7FA0, 0x7303,
    };
    const struct mode *md = &modes[m];

    wr(R_PIPELINE, 0x00);
    wr(0x02, 0x00); wr(0x03, md->r03); wr(0x04, 0x00); wr(0x05, md->r05); wr(0x0A, 0x28);
    wr(0x90, md->width >> 8); wr(0x91, (uint8_t)md->width);
    wr(0x92, md->height >> 8); wr(0x93, (uint8_t)md->height);
    wr(0x94, md->window_w >> 8); wr(0x95, (uint8_t)md->window_w);
    wr(0x96, md->window_h >> 8); wr(0x97, (uint8_t)md->window_h);
    wr(0x98, 0x01); wr(0x99, 0xF4);
    for (unsigned i = 0; i < sizeof(gains); i++) {
        wr((uint8_t)(0x45 + i), gains[i]);
        wr((uint8_t)(0x56 + i), gains[i]);
    }
    write_list(colour, sizeof(colour) / sizeof(colour[0]));
    wr(0x08, 0x00); wr(0xB0, 0x08); wr(0xB1, md->rb1);
    wr(0xB2, fine ? 0x39 : 0x38);   /* the card's fine or normal quantisation */
    write_list(windows, sizeof(windows) / sizeof(windows[0]));
    wr(R_PIPELINE, 0x03);
}

/* Sensor timing per mode, then one exposure step (the CE driver's index
 * 70 of 108, its starting point) with its default white balance. */
static void program_sensor(unsigned m)
{
    struct { uint8_t reg, value; } common[] = {
        {0x1C, 0x00}, {0x05, 0x51}, {0x02, 0x00}, {0x03, 0xFB}, {0x04, 0x00}, {0x06, 0x09},
    };
    for (unsigned i = 0; i < sizeof(common) / sizeof(common[0]); i++)
        sensor_write(common[i].reg, common[i].value);
    if (m != 1) {
        sensor_write(0x0F, 0x10); sensor_write(0x10, 0x12);
        sensor_write(0x11, 0x23); sensor_write(0x12, 0x12);
    }
    if (m == 1) {
        sensor_write(0x0B, 0x06); sensor_write(0x0A, 0x02);
        sensor_write(0x0D, 0xA8); sensor_write(0x0C, 0x7A);
        sensor_write(0x0E, 0xCC); sensor_write(0x19, 0xC8);
    } else if (m == 2) {
        sensor_write(0x0B, 0x06); sensor_write(0x0A, 0x02);
        sensor_write(0x0D, 0xA9); sensor_write(0x0C, 0x7B);
        sensor_write(0x0E, 0xC7); sensor_write(0x19, 0x14);
    } else {
        sensor_write(0x0B, 0x09); sensor_write(0x0A, 0x03);
        sensor_write(0x0D, 0xA8); sensor_write(0x0C, 0x7A);
        sensor_write(0x0E, 0xC7); sensor_write(0x19, 0x14);
    }
    sensor_write(0x0F, 0x1C); sensor_write(0x12, 0x1C);     /* gain */
    sensor_write(0x10, 0x22); sensor_write(0x11, 0x35);     /* white balance */
    sensor_write(0x14, 0x01); sensor_write(0x13, 0x11);     /* exposure */
    sensor_write(0x15, 0x00);
    sensor_write(0x1B, 0x0C);
}

/* ---- capture ------------------------------------------------------------ */

#define VEO_BUFFER  (128u * 1024u)
static uint8_t scan[VEO_BUFFER];
static struct veo_probe_status cap;
static uint16_t last[3];
static uint64_t cap_start;

static void restart_encoder(void)
{
    wr(R_CONTROL, 0x00);
    wr(R_PIPELINE, 0x00);
    VEO_DELAY_US(1000);
    wr(R_PIPELINE, 0x03);
    wr(R_CONTROL, 0x80);
}

static void start(unsigned m, bool fine)
{
    memset(&cap, 0, sizeof(cap));
    bring_up(true);
    wr(R_CONTROL, 0x00);
    wr(R_OUTPUT, 0xFF);
    sensor_write(0x1C, 0x21);
    VEO_DELAY_US(50000);
    program_mode(m, fine);
    program_sensor(m);
    wr(R_OUTPUT, 0xFF);
    wr(R_CONTROL, 0x80);
    sensor_write(0x1C, 0x04);
    cap.state = VEO_CAP_SYNC;
    cap_start = time_us();
}

/* The CE driver's frame loop: skip to the first 0xFFFF, then keep words
 * until the next; a frame must end FFD9 (either lane order) then 00FF. */
static void poll(void)
{
    uint64_t t0 = time_us();

    while (cap.state == VEO_CAP_SYNC || cap.state == VEO_CAP_DATA) {
        if (time_us() - t0 > 20000u)
            break;
        if (time_us() - cap_start > 5000000u) { cap.state = VEO_CAP_FAILED; break; }
        uint8_t status = rd(R_STATUS);
        if (status & ST_ABORT) {
            restart_encoder();
            cap.restarts++;
            cap.state = VEO_CAP_SYNC;
            continue;
        }
        if (!(status & ST_DATA))
            continue;
        for (unsigned i = 0; i < 256; i++) {
            uint16_t w = fifo();
            if (cap.state == VEO_CAP_SYNC) {
                if (w == 0xFFFF) {
                    cap.state = VEO_CAP_DATA;
                    cap.bytes = cap.words = 0;
                }
                continue;
            }
            if (w == 0xFFFF) {
                cap.tail[0] = last[0]; cap.tail[1] = last[1]; cap.tail[2] = last[2];
                if (last[2] == 0x00FF && (last[1] == 0xFFD9 || last[1] == 0xD9FF)) {
                    wr(R_CONTROL, 0x00);
                    wr(R_PIPELINE, 0x00);
                    cap.state = VEO_CAP_DONE;
                } else {
                    cap.resyncs++;
                    cap.state = VEO_CAP_DATA;
                    cap.bytes = cap.words = 0;
                }
                break;
            }
            last[0] = last[1]; last[1] = last[2]; last[2] = w;
            cap.words++;
            if (cap.bytes + 2 <= VEO_BUFFER) {
                scan[cap.bytes++] = (uint8_t)(w >> 8);
                scan[cap.bytes++] = (uint8_t)w;
            }
        }
    }
    cap.elapsed_ms = (uint32_t)((time_us() - cap_start) / 1000u);
}

/* Bridge registers worth seeing before anything is written. */
static const uint8_t probe_regs[VEO_PROBE_REG_COUNT] = {
    0x00, 0x02, 0x03, 0x04, 0x05, 0x08, 0x0A, 0x0C, 0x0D, 0x0F, 0x11, 0x19,
    0x1F, 0x20, 0x26, 0x27, 0x2A, 0x2B, 0x2C, 0x2D, 0x45, 0x56, 0x67, 0x70,
    0x71, 0x72, 0x73, 0x80, 0x81, 0x82, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
    0x9A, 0xB0, 0xB1, 0xB2, 0xB3, 0xE1, 0xE2, 0xE3, 0xF0, 0xFE, 0xFF, 0x01,
};

int sys_cam_probe(struct proc *p, uint32_t op, uint32_t arg, uint32_t ubuf, uint32_t len)
{
    if (!(p->flags & PROC_USB)) return GZ_EPERM;
    if (!PCMCIA_CARD_PRESENT()) return GZ_ENOENT;
    if (!cf_card_is_veo()) {
        if (!cf_card_is_icsi_camera()) return GZ_ENOENT;
        if (op != VEO_PROBE_IO && op != VEO_PROBE_IO_BLOCK && op != VEO_PROBE_ATTR)
            return GZ_EINVAL;
        cf_probe_hold();
    }

    switch (op) {
    case VEO_PROBE_IO: {
        uint32_t pa = VEO_IO_WINDOW + (arg & VEO_IO_PORT_MASK);
        bool wide = arg & VEO_IO_16;
        if (wide && (pa & 1)) return GZ_EINVAL;
        if (arg & VEO_IO_WRITE) {
            if (wide) VEO_IO_W16(pa, (uint16_t)(arg >> 16));
            else VEO_IO_W8(pa, (uint8_t)(arg >> 16));
            return 0;
        }
        return wide ? VEO_IO_R16(pa) : VEO_IO_R8(pa);
    }
    case VEO_PROBE_IO_BLOCK: {
        uint32_t pa = VEO_IO_WINDOW + (arg & VEO_IO_PORT_MASK);
        bool wide = arg & VEO_IO_16;
        uint8_t buf[512];
        if ((wide && (pa & 1)) || len > sizeof(buf) || (wide && (len & 1))) return GZ_EINVAL;
        for (uint32_t i = 0; i < len; i += wide ? 2 : 1) {
            if (wide) { uint16_t v = VEO_IO_R16(pa); buf[i] = (uint8_t)v; buf[i + 1] = (uint8_t)(v >> 8); }
            else buf[i] = VEO_IO_R8(pa);
        }
        int r = copy_to_as(&p->as, ubuf, buf, len);
        return r ? r : (int)len;
    }
    case VEO_PROBE_SETUP: {
        uint32_t base = arg & 0xFFFF, a020 = arg >> 16;
        if (base > 0x3F0 || (base & 0xF) || a020 > 2) return GZ_EINVAL;
        io_base = base;
        base_pa = VEO_IO_WINDOW + base;
        if (a020 == 1) PCMCIA_A020 = PCMCIA_A020 | PCMCIA_A020_CONFIG;
        if (a020 == 2) PCMCIA_A020 = PCMCIA_A020 & ~PCMCIA_A020_CONFIG;
        kprintf("veo: I/O base %x, A020 %x\n", io_base, PCMCIA_A020);
        return 0;
    }
    case VEO_PROBE_REGS: {
        struct veo_probe_regs r;
        if (len != sizeof(r)) return GZ_EINVAL;
        memset(&r, 0, sizeof(r));
        r.base = io_base;
        r.a020 = PCMCIA_A020;
        for (unsigned i = 0; i < VEO_PROBE_REG_COUNT; i++) {
            r.reg_index[i] = probe_regs[i];
            r.reg[i] = rd(probe_regs[i]);
        }
        /* The sensor sequencer's first data slot is a scratch register:
         * a write and read back shows whether 16-bit cycles reach the card. */
        r.slot_write[0] = 0x5A; wr(0x27, 0x5A); r.slot_read[0] = rd(0x27);
        r.slot_write[1] = 0xA5; wr(0x27, 0xA5); r.slot_read[1] = rd(0x27);
        for (unsigned i = 0; i < 8; i++)
            r.port[i] = VEO_IO_R16(base_pa + 2 * i);
        return copy_to_as(&p->as, ubuf, &r, sizeof(r));
    }
    case VEO_PROBE_INIT:
        return bring_up(arg != 0);
    case VEO_PROBE_SENSOR:
        if (arg > 0x7F) return GZ_EINVAL;
        return sensor_read((uint8_t)arg);
    case VEO_PROBE_POKE:
        wr((uint8_t)(arg >> 8), (uint8_t)arg);
        return 0;
    case VEO_PROBE_PEEK:
        return rd((uint8_t)arg);
    case VEO_PROBE_SENSOR_WRITE:
        if ((arg >> 8) > 0x7F) return GZ_EINVAL;
        return sensor_write((uint8_t)(arg >> 8), (uint8_t)arg) ? 0 : GZ_ETIMEDOUT;
    case VEO_PROBE_BUS_DELAY:
        if (arg > 100000u) return GZ_EINVAL;
        bus_delay_us = arg;
        return 0;
    case VEO_PROBE_ATTR: {
        /* Attribute memory with the socket's configuration window mapped,
         * as cf.c's configure() does for the COR. */
        uint32_t off = arg & 0xFFFF;
        if (off >= 0x800) return GZ_EINVAL;
        uint32_t a020 = PCMCIA_A020;
        PCMCIA_A020 = (a020 & ~0x04u) | PCMCIA_A020_CONFIG;
        if (arg >> 24) VEO_ATTR8(off) = (uint8_t)(arg >> 16);
        int v = VEO_ATTR8(off);
        PCMCIA_A020 = a020;
        return v;
    }
    case VEO_PROBE_START: {
        unsigned m = arg & 0xFF;
        if (m < 1 || m > 3) return GZ_EINVAL;
        start(m, (arg >> 8) & 1);
        return 0;
    }
    case VEO_PROBE_POLL:
        if (len != sizeof(cap)) return GZ_EINVAL;
        poll();
        return copy_to_as(&p->as, ubuf, &cap, sizeof(cap));
    case VEO_PROBE_READ: {
        if (cap.state != VEO_CAP_DONE) return GZ_EIO;
        uint32_t end = cap.bytes < VEO_BUFFER ? cap.bytes : VEO_BUFFER;
        if (arg >= end) return 0;
        uint32_t n = end - arg < len ? end - arg : len;
        int r = copy_to_as(&p->as, ubuf, scan + arg, n);
        return r ? r : (int)n;
    }
    }
    return GZ_EINVAL;
}
#endif
