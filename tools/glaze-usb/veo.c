/* Veo Photo Traveler bring-up over USB (VEO_PROBE=1 kernels; docs/hardware/VEO.md). */
#include "client.h"
#include <glaze/abi.h>
#include <glaze/veo.h>
#include <glaze/media.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* One probe call: returns the kernel's result, and the op's bytes in data and len. */
static int probe(struct gu_client *c, uint32_t op, uint32_t arg, uint32_t want,
                 const uint8_t **data, uint32_t *len)
{
    uint8_t req[12];
    gu_put32(req, op); gu_put32(req + 4, arg); gu_put32(req + 8, want);
    int n = gu_call(c, GU_CAM_PROBE, req, sizeof(req));
    if (n < 4) { fprintf(stderr, "veo: USB call failed (%d)\n", n); return -1000; }
    int32_t r = (int32_t)gu_get32(c->frame + GU_HEADER);
    if (data) *data = c->frame + GU_HEADER + 4;
    if (len) *len = (uint32_t)n - 4;
    return r;
}

static int regs(struct gu_client *c, int left, char **argv)
{
    uint32_t base = left > 0 ? (uint32_t)strtoul(argv[0], NULL, 0) : 0x300;
    uint32_t a020 = left > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 0;
    int r = probe(c, VEO_PROBE_SETUP, base | a020 << 16, 0, NULL, NULL);
    if (r) { fprintf(stderr, "veo: setup failed (%d)%s\n", r,
                     r == GZ_EINVAL ? "; reload a VEO_PROBE=1 build" :
                     r == GZ_ENOENT ? "; no Veo card in the socket" : ""); return 1; }
    const uint8_t *p; uint32_t n;
    r = probe(c, VEO_PROBE_REGS, 0, 0, &p, &n);
    if (r || n != sizeof(struct veo_probe_regs)) { fprintf(stderr, "veo: regs failed (%d)\n", r); return 1; }
    struct veo_probe_regs v;
    memcpy(&v, p, sizeof(v));
    printf("I/O base %03x, A020 %08x\n", v.base, v.a020);
    printf("slot 0x27: wrote %02x read %02x, wrote %02x read %02x -> %s\n",
           v.slot_write[0], v.slot_read[0], v.slot_write[1], v.slot_read[1],
           v.slot_read[0] == v.slot_write[0] && v.slot_read[1] == v.slot_write[1]
               ? "16-bit register access works" : "no read-back");
    printf("ports:");
    for (unsigned i = 0; i < 8; i++) printf(" +%u=%04x", 2 * i, v.port[i]);
    putchar('\n');
    for (unsigned i = 0; i < VEO_PROBE_REG_COUNT; i++)
        printf("%s%02x=%02x", i % 12 ? "  " : (i ? "\n" : ""), v.reg_index[i], v.reg[i]);
    putchar('\n');
    return 0;
}

static int grab(struct gu_client *c, const char *mode_arg, const char *quality, const char *path)
{
    unsigned mode = (unsigned)atoi(mode_arg);
    int fine = !strcmp(quality, "fine");
    if (mode < 1 || mode > 3 || (!fine && strcmp(quality, "normal"))) {
        fputs("veo-grab: MODE 1 (640x480), 2 (320x240) or 3 (160x120); QUALITY fine or normal\n", stderr);
        return 1;
    }
    int r = probe(c, VEO_PROBE_START, mode | (unsigned)fine << 8, 0, NULL, NULL);
    if (r) { fprintf(stderr, "veo: start failed (%d)\n", r); return 1; }
    struct veo_probe_status st = {0};
    for (int i = 0; i < 400; i++) {
        const uint8_t *p; uint32_t n;
        r = probe(c, VEO_PROBE_POLL, 0, 0, &p, &n);
        if (r || n != sizeof(st)) { fprintf(stderr, "veo: poll failed (%d)\n", r); return 1; }
        memcpy(&st, p, sizeof(st));
        if (st.state == VEO_CAP_DONE || st.state == VEO_CAP_FAILED) break;
    }
    printf("state=%u bytes=%u words=%u restarts=%u resyncs=%u %ums tail=%04x %04x %04x\n",
           st.state, st.bytes, st.words, st.restarts, st.resyncs, st.elapsed_ms,
           st.tail[0], st.tail[1], st.tail[2]);
    if (st.state != VEO_CAP_DONE) return 1;

    uint8_t *jpg = malloc(MEDIA_VEO_HEADER_SIZE + st.bytes);
    if (!jpg) return 1;
    int h = media_veo_jpeg_header(jpg, MEDIA_VEO_HEADER_SIZE, mode, fine);
    uint32_t got = 0;
    while (got < st.bytes) {
        const uint8_t *p; uint32_t n;
        r = probe(c, VEO_PROBE_READ, got, GU_PAYLOAD - 4, &p, &n);
        if (r <= 0 || (uint32_t)r != n) { fprintf(stderr, "veo: read at %u failed (%d)\n", got, r); free(jpg); return 1; }
        memcpy(jpg + h + got, p, n);
        got += n;
    }
    char raw[1024];
    snprintf(raw, sizeof(raw), "%s.scan", path);
    FILE *f = fopen(raw, "wb"), *g = fopen(path, "wb");
    int ok = f && g && fwrite(jpg + h, 1, got, f) == got && fwrite(jpg, 1, h + got, g) == h + got;
    if (f) fclose(f);
    if (g) fclose(g);
    free(jpg);
    printf("%s: %u-byte scan, %s (header %d bytes); raw scan in %s\n", ok ? "saved" : "failed",
           got, path, h, raw);
    return !ok;
}

/* Raw socket I/O from a script, one USB session (VEO_PROBE_IO). Ports
 * are offsets in the socket's I/O space, e.g. 0x170-0x177 and 0x376 for
 * CF ATA on the secondary IDE ports. */
static int io_script(struct gu_client *c, const char *path)
{
    FILE *f = strcmp(path, "-") ? fopen(path, "r") : stdin;
    char line[256];
    int line_no = 0, failed = 0;
    if (!f) { perror(path); return 1; }
    while (fgets(line, sizeof(line), f)) {
        char op[16];
        unsigned long a = 0, b = 0, d = 0, e = 0;
        line_no++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        int n = sscanf(line, "%15s %li %li %li %li", op, (long *)&a, (long *)&b, (long *)&d, (long *)&e);
        if (n < 1) continue;
        int r;
        if ((!strcmp(op, "r8") || !strcmp(op, "r16")) && n == 2) {
            r = probe(c, VEO_PROBE_IO, (uint32_t)(a & VEO_IO_PORT_MASK) | (op[1] == '1' ? VEO_IO_16 : 0), 0, NULL, NULL);
            printf("%s %03lx = %0*x\n", op, a, op[1] == '1' ? 4 : 2, r < 0 ? 0 : r);
        } else if ((!strcmp(op, "w8") || !strcmp(op, "w16")) && n == 3) {
            r = probe(c, VEO_PROBE_IO, (uint32_t)(a & VEO_IO_PORT_MASK) | VEO_IO_WRITE |
                      (op[1] == '1' ? VEO_IO_16 : 0) | (uint32_t)(b & 0xFFFF) << 16, 0, NULL, NULL);
            printf("%s %03lx <- %lx\n", op, a, b);
        } else if (!strcmp(op, "attr") && (n == 2 || n == 3)) {
            uint32_t arg = (uint32_t)a | (n == 3 ? ((uint32_t)b & 0xFF) << 16 | 1u << 24 : 0);
            r = probe(c, VEO_PROBE_ATTR, arg, 0, NULL, NULL);
            printf("attr %03lx%s = %02x\n", a, n == 3 ? " (written)" : "", r & 0xFF);
        } else if (!strcmp(op, "block") && (n == 3 || n == 4)) {
            int wide = n == 4 && b && d == 16;
            uint32_t len = (uint32_t)b * (wide ? 2 : 1);
            const uint8_t *p; uint32_t got;
            r = probe(c, VEO_PROBE_IO_BLOCK, (uint32_t)(a & VEO_IO_PORT_MASK) | (wide ? VEO_IO_16 : 0), len, &p, &got);
            printf("block %03lx x%lu%s:", a, b, wide ? " (16-bit)" : "");
            for (uint32_t i = 0; r > 0 && i < got && i < (uint32_t)r; i++)
                printf("%s%02x", i % 32 ? " " : "\n  ", p[i]);
            putchar('\n');
        } else if (!strcmp(op, "wait") && (n == 4 || n == 5)) {
            unsigned ms = n == 5 ? (unsigned)e : 1000, waited = 0;
            do {
                r = probe(c, VEO_PROBE_IO, (uint32_t)(a & VEO_IO_PORT_MASK), 0, NULL, NULL);
                if (r >= 0 && ((unsigned)r & b) == d) break;
                usleep(2000); waited += 2;
            } while (waited < ms);
            printf("wait %03lx & %02lx == %02lx: %s (%02x after %u ms)\n", a, b, d,
                   r >= 0 && ((unsigned)r & b) == d ? "ok" : "TIMEOUT", r & 0xFF, waited);
            if (r < 0) failed = 1;
            continue;
        } else if (!strcmp(op, "sleep") && n == 2) {
            usleep((useconds_t)a * 1000); continue;
        } else {
            fprintf(stderr, "%s:%d: bad line\n", path, line_no); failed = 1; break;
        }
        if (r < 0 && r > -1000) { fprintf(stderr, "  error %d\n", r); failed = 1; break; }
        if (r == -1000) { failed = 1; break; }
    }
    if (f != stdin) fclose(f);
    return failed;
}

int gu_veo(struct gu_client *c, const char *cmd, int left, char **argv)
{
    if (!(c->caps & GU_CAP_CAM_PROBE)) { fputs("Reload a build with Veo camera probing first.\n", stderr); return 1; }
    if (!strcmp(cmd, "veo-regs")) return regs(c, left, argv);
    if (!strcmp(cmd, "veo-io")) return io_script(c, argv[0]);
    if (!strcmp(cmd, "veo-sensor")) {
        int r = probe(c, VEO_PROBE_SENSOR, (uint32_t)strtoul(argv[0], NULL, 0), 0, NULL, NULL);
        printf("sensor %s = %d (0x%02x)\n", argv[0], r, r & 0xFF);
        return r < 0;
    }
    if (!strcmp(cmd, "veo-poke") || !strcmp(cmd, "veo-swrite")) {
        uint32_t reg = (uint32_t)strtoul(argv[0], NULL, 0), v = (uint32_t)strtoul(argv[1], NULL, 0);
        int r = probe(c, cmd[4] == 'p' ? VEO_PROBE_POKE : VEO_PROBE_SENSOR_WRITE, reg << 8 | (v & 0xFF), 0, NULL, NULL);
        printf("%s %02x = %02x: %d\n", cmd + 4, reg, v & 0xFF, r);
        return r != 0;
    }
    if (!strcmp(cmd, "veo-peek")) {
        int r = probe(c, VEO_PROBE_PEEK, (uint32_t)strtoul(argv[0], NULL, 0), 0, NULL, NULL);
        printf("bridge %s = %02x\n", argv[0], r & 0xFF);
        return r < 0;
    }
    if (!strcmp(cmd, "veo-delay")) {
        int r = probe(c, VEO_PROBE_BUS_DELAY, (uint32_t)strtoul(argv[0], NULL, 0), 0, NULL, NULL);
        printf("bus delay %s us: %d\n", argv[0], r);
        return r != 0;
    }
    if (!strcmp(cmd, "veo-attr")) {
        uint32_t off = (uint32_t)strtoul(argv[0], NULL, 0);
        uint32_t arg = off | (left > 1 ? ((uint32_t)strtoul(argv[1], NULL, 0) & 0xFF) << 16 | 1u << 24 : 0);
        int r = probe(c, VEO_PROBE_ATTR, arg, 0, NULL, NULL);
        printf("attribute %03x = %02x (%d)\n", off, r & 0xFF, r);
        return r < 0;
    }
    if (!strcmp(cmd, "veo-init")) {
        int r = probe(c, VEO_PROBE_INIT, left == 0 || strcmp(argv[0], "preview"), 0, NULL, NULL);
        printf("sensor ID %d (0x%02x): %s\n", r, r & 0xFF,
               r == 0x18 ? "camera responding" : "unexpected (want 0x18)");
        return r != 0x18;
    }
    return grab(c, argv[0], argv[1], argv[2]);
}
