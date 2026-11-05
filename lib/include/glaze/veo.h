/* Veo Photo Traveler CF camera: bring-up probe ABI (docs/hardware/VEO.md).
 * SYS_CAM_PROBE is PROC_USB only and exists in VEO_PROBE=1 kernels;
 * elsewhere it returns GZ_EINVAL. */
#ifndef GLAZE_VEO_H
#define GLAZE_VEO_H

#include <stdint.h>

enum {
    VEO_PROBE_SETUP,    /* arg: I/O base | A020 mode << 16 (0 keep, 1 set bit 3, 2 clear) */
    VEO_PROBE_REGS,     /* buf: struct veo_probe_regs */
    VEO_PROBE_INIT,     /* arg: 1 still, 0 preview; returns the sensor's ID register */
    VEO_PROBE_START,    /* arg: mode (1 640x480, 2 320x240, 3 160x120) | quality << 8 */
    VEO_PROBE_POLL,     /* ~20 ms of FIFO work; buf: struct veo_probe_status */
    VEO_PROBE_READ,     /* arg: byte offset; buf/len: scan bytes, returns the count */
    VEO_PROBE_SENSOR,   /* arg: sensor register; returns its value (serial bus) */
    VEO_PROBE_POKE,     /* arg: bridge reg << 8 | value */
    VEO_PROBE_PEEK,     /* arg: bridge reg; returns its value */
    VEO_PROBE_SENSOR_WRITE, /* arg: sensor reg << 8 | value */
    VEO_PROBE_ATTR,     /* arg: attribute offset | value << 16 | write << 24; returns the byte */
    VEO_PROBE_BUS_DELAY,/* arg: microseconds to wait after each sequencer run */
    /* Raw socket I/O, also allowed for the Ambicom DGCAM-CF (CIS "ICSI
     * EMULATION"), whose register protocol is unknown. */
    VEO_PROBE_IO,       /* arg: port | VEO_IO_16 | VEO_IO_WRITE | value << 16; returns the read */
    VEO_PROBE_IO_BLOCK, /* arg: port | VEO_IO_16; buf/len: len / width reads of that port */
};

#define VEO_IO_PORT_MASK    0x0FFFu     /* offset in the socket's I/O space */
#define VEO_IO_16           0x1000u
#define VEO_IO_WRITE        0x2000u

enum { VEO_CAP_IDLE, VEO_CAP_SYNC, VEO_CAP_DATA, VEO_CAP_DONE, VEO_CAP_FAILED };

#define VEO_PROBE_REG_COUNT 48

struct veo_probe_regs {
    uint32_t base, a020;
    uint8_t slot_write[2], slot_read[2];    /* bridge reg 0x27 write/read back */
    uint16_t port[8];                       /* raw 16-bit reads of B+0..B+14 */
    uint8_t reg[VEO_PROBE_REG_COUNT];       /* values of veo_probe_reg_list */
    uint8_t reg_index[VEO_PROBE_REG_COUNT];
};

struct veo_probe_status {
    uint32_t state, bytes, words, restarts, resyncs, elapsed_ms;
    uint16_t tail[4];                       /* the last FIFO words before 0xFFFF */
};

#endif
