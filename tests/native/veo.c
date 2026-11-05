/* Veo CF camera probe (kernel/drivers/veo.c) against a register model of
 * the card, and the JPEG header for its scans (lib/media/veo_jpeg.c). */
#include "../../kernel/kernel.h"
#include "../../kernel/drivers.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "../../third_party/stb/stb_image.h"
#include <glaze/media.h>

/* ---- the card model ---- */
static uint8_t bridge[256], sensor[128], index_reg;
static uint16_t writes[512];
static unsigned nwrites;
static const uint16_t *stream;
static unsigned stream_len, stream_at;
static int bus_state, bus_reg = -1, bus_reading;

static void sequencer(void)
{
    static const uint8_t slots[5] = {0x27, 0x2D, 0x2E, 0x2F, 0x30};
    uint16_t ops = (uint16_t)(bridge[0x1F] << 8 | bridge[0x20]);
    for (unsigned i = 0; i < 5; i++) {
        unsigned op = (ops >> (3 * i)) & 7;
        uint8_t byte = bridge[slots[i]];
        if (op == 1) bus_state = 1;                 /* start: address next */
        else if (op == 2) bus_state = 0;
        else if (op == 3) {
            if (bus_state == 1) { bus_reading = byte == 0xAB; assert(byte == 0xAA || byte == 0xAB); bus_state = 2; }
            else if (bus_state == 2) { bus_reg = byte >> 1; bus_state = 3; }
            else if (bus_state == 3) { assert(bus_reg >= 0 && bus_reg < 128); sensor[bus_reg++] = byte; }
        } else if (op == 6) {
            assert(bus_reading && bus_reg >= 0);
            bridge[0x26] = sensor[bus_reg];
        }
    }
}

static void port_write(uint32_t pa, uint16_t v)
{
    unsigned off = pa & 0xF;
    assert((pa & ~0xFu) == 0x0A00C300u);
    if (off == 4) { index_reg = (uint8_t)(v >> 8); return; }
    assert(off == 6);
    if (nwrites < sizeof(writes) / sizeof(writes[0]))
        writes[nwrites++] = v;
    uint8_t reg = (uint8_t)(v >> 8);
    bridge[reg] = (uint8_t)v;
    if (reg == 0x1F && (v & 0x80)) sequencer();
}

static uint16_t port_read(uint32_t pa)
{
    unsigned off = pa & 0xF;
    if (off == 0) return stream_at < stream_len ? stream[stream_at++] : 0xFFFF;
    assert(off == 4);
    if (index_reg == 0x0D)  /* sequencer idle; data while the stream lasts */
        return stream_at < stream_len ? 0x20 : 0x00;
    return bridge[index_reg];
}

#define GLAZE_HW_H
#define GLAZE_VEO_PROBE
static uint32_t mock_a020, status;
#define PCMCIA_A020 mock_a020
#define PCMCIA_A020_CONFIG 8u
#define PCMCIA_STATUS status
#define PCMCIA_CARD_PRESENT() ((PCMCIA_STATUS & 6u) == 0)
#define VEO_IO_R16(pa) port_read(pa)
#define VEO_IO_W16(pa, v) port_write((pa), (uint16_t)(v))
static uint8_t io8[0x1000];
#define VEO_IO_R8(pa) io8[(pa) & 0xFFF]
#define VEO_IO_W8(pa, v) (io8[(pa) & 0xFFF] = (uint8_t)(v))
#define VEO_DELAY_US(us) ((void)(us))
static uint8_t attr[0x800];
#define VEO_ATTR8(off) attr[off]
#include "../../kernel/drivers/veo.c"

uint64_t time_us(void) { static uint64_t now; return now += 50; }
void kprintf(const char *format, ...) { (void)format; }
static bool is_veo = true, is_icsi, held;
bool cf_card_is_veo(void) { return is_veo; }
bool cf_card_is_icsi_camera(void) { return is_icsi; }
void cf_probe_hold(void) { held = true; }
static uint8_t user_mem[4096];    /* user addresses are offsets into it */
int copy_to_as(struct addrspace *as, uint32_t uva, const void *src, uint32_t len)
{
    (void)as;
    if (uva + len > sizeof(user_mem)) return GZ_EINVAL;
    memcpy(user_mem + uva, src, len);
    return 0;
}

static void sensor_write_encoding(void)
{
    nwrites = 0;
    assert(sensor_write(0x1C, 0x21));
    /* START, SEND AA, ACK, SEND 1C<<1, ACK: slots 1-4 loaded, then the
     * opcodes 1,3,4,3,4 with bit 15 set run the first batch. */
    const uint16_t first[] = {0x2DAA, 0x2EFF, 0x2F38, 0x30FF, 0x1F00, 0x2019, 0x1FC7};
    assert(nwrites >= 7 && !memcmp(writes, first, sizeof(first)));
    /* SEND 21, ACK, STOP, padded with 7s. */
    const uint16_t second[] = {0x2721, 0x2DFF, 0x1F00, 0x20A3, 0x1FFE};
    assert(nwrites == 12 && !memcmp(writes + 7, second, sizeof(second)));
    assert(sensor[0x1C] == 0x21);
}

static void bring_up_reads_sensor_id(void)
{
    memset(sensor, 0, sizeof(sensor));
    sensor[0] = SENSOR_ID;
    assert(sensor_read(0) == SENSOR_ID);
    assert(bring_up(true) == SENSOR_ID);
    assert(bridge[0x19] & 0x04);               /* still: bit 2 set */
    assert(sensor[0x1C] == 0);
    sensor[0] = 0x55;
    assert(bring_up(false) == 0x55);
    assert(!(bridge[0x19] & 0x04));
}

static void capture_frame(void)
{
    static const uint16_t frame[] = {
        0x1234, 0x5678, 0xFFFF,                /* the tail of an earlier frame */
        0xA1B2, 0xC3D4, 0xE5F6, 0xFFD9, 0x00FF, 0xFFFF,
    };
    struct proc p = {0};
    struct veo_probe_status st;
    const uint32_t ST = 0, OUT = 256;

    p.flags = PROC_USB;
    stream = frame; stream_len = sizeof(frame) / 2; stream_at = 0;
    sensor[0] = SENSOR_ID;
    assert(sys_cam_probe(&p, VEO_PROBE_START, 3 | 1 << 8, 0, 0) == 0);
    assert(bridge[0x91] == 160 && bridge[0x93] == 120 && bridge[0xB2] == 0x39);
    assert(sys_cam_probe(&p, VEO_PROBE_READ, 0, OUT, 32) == GZ_EIO);
    for (int i = 0; i < 10 && cap.state != VEO_CAP_DONE; i++)
        assert(!sys_cam_probe(&p, VEO_PROBE_POLL, 0, ST, sizeof(st)));
    memcpy(&st, user_mem + ST, sizeof(st));
    assert(st.state == VEO_CAP_DONE && st.words == 5 && st.bytes == 10);
    int n = sys_cam_probe(&p, VEO_PROBE_READ, 0, OUT, 32);
    const uint8_t want[] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0xFF, 0xD9, 0x00, 0xFF};
    assert(n == 10 && !memcmp(user_mem + OUT, want, sizeof(want)));
    p.flags = 0;
    assert(sys_cam_probe(&p, VEO_PROBE_POLL, 0, 0, 0) == GZ_EPERM);
}

/* The Ambicom (ICSI) card gets raw I/O and attribute access only. */
static void icsi_raw_io(void)
{
    struct proc p = {0};
    p.flags = PROC_USB;
    is_veo = false; is_icsi = true;
    assert(sys_cam_probe(&p, VEO_PROBE_INIT, 1, 0, 0) == GZ_EINVAL && !held);
    assert(sys_cam_probe(&p, VEO_PROBE_IO, 0x177 | VEO_IO_WRITE | 0xEC << 16, 0, 0) == 0 && held);
    assert(io8[0x177] == 0xEC && sys_cam_probe(&p, VEO_PROBE_IO, 0x177, 0, 0) == 0xEC);
    io8[0x170] = 0x42;
    assert(sys_cam_probe(&p, VEO_PROBE_IO_BLOCK, 0x170, 0, 4) == 4 && !memcmp(user_mem, "BBBB", 4));
    assert(sys_cam_probe(&p, VEO_PROBE_IO, 0x171 | VEO_IO_16, 0, 0) == GZ_EINVAL);
    is_icsi = false;
    assert(sys_cam_probe(&p, VEO_PROBE_IO, 0x177, 0, 0) == GZ_ENOENT);
    is_veo = true;
}

static void jpeg_header(void)
{
    uint8_t h[MEDIA_VEO_HEADER_SIZE];
    for (unsigned mode = 1; mode <= 3; mode++)
        for (int fine = 0; fine < 2; fine++) {
            int x, y, comp;
            assert(media_veo_jpeg_header(h, sizeof(h), mode, fine) == MEDIA_VEO_HEADER_SIZE);
            assert(stbi_info_from_memory(h, sizeof(h), &x, &y, &comp));
            assert(x == 640 >> (mode - 1) && y == 480 >> (mode - 1) && comp == 3);
        }
    assert(media_veo_jpeg_header(h, sizeof(h), 4, 0) < 0);
    assert(media_veo_jpeg_header(h, sizeof(h) - 1, 1, 0) < 0);
}

int main(void)
{
    sensor_write_encoding();
    bring_up_reads_sensor_id();
    capture_frame();
    icsi_raw_io();
    jpeg_header();
    puts("veo-test: ok");
    return 0;
}
