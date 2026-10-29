#include "client.h"
#include "pcconnect.h"
#include <glaze/usb_boot.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <errno.h>

struct boot_options { uint32_t ramfs_kb, flags; };
static void erase(void *p, size_t n)
{
    volatile uint8_t *b = p;
    while (n--) *b++ = 0;
}
static int boot_options(const char *dir, struct boot_options *o)
{
    o->ramfs_kb = 2048; o->flags = 2;
    char path[1024], line[512];
    if (snprintf(path, sizeof(path), "%s/glaze.cfg", dir) >= (int)sizeof(path)) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    int result = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        char *end = p + strcspn(p, "\r\n#");
        while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
        *end = 0;
        if (!strncmp(p, "ramfs_kb=", 9)) {
            char *tail; unsigned long n = strtoul(p + 9, &tail, 10);
            if (*tail || n < 128 || n > 11264) { result = -1; break; }
            o->ramfs_kb = (uint32_t)n;
        } else if (!strncmp(p, "nand=", 5)) {
            if (!strcmp(p + 5, "off")) o->flags = 2;
            else if (!strcmp(p + 5, "ro")) o->flags = 0;
            else { fprintf(stderr, "USB boot accepts nand=off or nand=ro\n"); result = -1; break; }
        } else if ((!strncmp(p, "kernel=", 7) && strcmp(p + 7, "kernel.bin")) ||
                   (!strncmp(p, "romfs=", 6) && strcmp(p + 6, "romfs.img"))) {
            fprintf(stderr, "USB artifact directories use kernel.bin and romfs.img\n"); result = -1; break;
        }
    }
    if (ferror(f)) result = -1;
    fclose(f); return result;
}

static uint64_t now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (unsigned)t.tv_nsec / 1000000;
}
int gu_call(struct gu_client *c, uint32_t op, const void *data, uint32_t n)
{
    uint8_t out[GU_FRAME], in[4096];
    uint32_t id = ++c->id;
    size_t size = gu_encode(out, op, id, 0, data, n);
    if (!size || !id) return -1;
    int r = usb_write(c->usb, out, size, 5000);
    erase(out, size);
    if (r) return r;
    uint64_t end = now() + 10000;
    while (now() < end && !usb_cancelled) {
        r = usb_read(c->usb, in, sizeof(in), 250);
        if (r == LIBUSB_ERROR_TIMEOUT) continue;
        if (r < 0) return r;
        for (int i = 0; i < r; i++) {
            if (gu_feed(&c->parser, in[i]) != 1) continue;
            const uint8_t *p = c->parser.bytes;
            uint32_t reply_op = gu_get32(p + 4) >> 16;
            if (reply_op == GU_LOG && !gu_get32(p + 8)) {
                uint32_t len = gu_get32(p + 12);
                if (len >= 4) {
                    uint32_t dropped = gu_get32(p + GU_HEADER);
                    if (dropped) fprintf(stderr, "[USB log: %u bytes dropped]\n", dropped);
                    fwrite(p + GU_HEADER + 4, 1, len - 4, stdout); fflush(stdout);
                }
                continue;
            }
            if (reply_op != (op | GU_REPLY) || gu_get32(p + 8) != id) continue;
            memcpy(c->frame, p, GU_HEADER + gu_get32(p + 12));
            int32_t status = (int32_t)gu_get32(p + 16);
            if (status) { fprintf(stderr, "Glaze USB op %u failed (%d)\n", op, status); return -1; }
            return (int)gu_get32(p + 12);
        }
    }
    fprintf(stderr, "Glaze USB op %u timed out\n", op); return -1;
}
int gu_hello(struct gu_client *c, struct usb_link *u)
{
    memset(c, 0, sizeof(*c)); c->usb = u;
    if (usb_lines(u, 3) < 0) return -1;
    int r = gu_call(c, GU_HELLO, NULL, 0);
    if (r != 44 || gu_get32(c->frame + GU_HEADER) != GU_VERSION) return -1;
    const uint8_t *b = c->frame + GU_HEADER;
    c->caps = gu_get32(b + 4);
    fprintf(stderr, "runtime=Glaze build=%.32s free=%u KiB capabilities=0x%x\n", b + 12, gu_get32(b + 8) / 1024, gu_get32(b + 4));
    if (c->caps & GU_CAP_ENTROPY) {
        uint8_t seed[32];
        if (getentropy(seed, sizeof(seed))) {
            perror("OS CSPRNG"); erase(seed, sizeof(seed)); return -1;
        }
        /* Packet tracing must never print the seed, even with --trace. */
        int trace = u->trace; u->trace = 0;
        r = gu_call(c, GU_ENTROPY, seed, sizeof(seed));
        u->trace = trace; erase(seed, sizeof(seed));
        if (r != 4 || gu_get32(c->frame + GU_HEADER) > 1) return -1;
        if (gu_get32(c->frame + GU_HEADER))
            fputs("Wi-Fi entropy seeded from host OS CSPRNG\n", stderr);
    }
    return 0;
}
static int file_crc(const char *path, uint32_t *size, uint32_t *crc)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    uint8_t b[4096]; uint32_t len = 0, sum = ~0u; size_t n;
    while ((n = fread(b, 1, sizeof(b), f))) {
        if (len > GU_RAM_BYTES - n) { fclose(f); return -1; }
        len += (uint32_t)n; sum = gu_crc(sum, b, n);
    }
    int r = ferror(f) ? -1 : 0; fclose(f);
    *size = len; *crc = ~sum; return r;
}
int gu_upload(struct gu_client *c, const char *local, const char *remote)
{
    uint8_t b[GU_PAYLOAD]; uint32_t size, crc;
    if (strlen(remote) > 120 || file_crc(local, &size, &crc)) return -1;
    gu_put32(b, size); gu_put32(b + 4, crc); memcpy(b + 8, remote, strlen(remote));
    if (gu_call(c, GU_PUT_BEGIN, b, (uint32_t)strlen(remote) + 8) < 0) return -1;
    FILE *f = fopen(local, "rb");
    if (!f) { gu_call(c, GU_ABORT, NULL, 0); return -1; }
    uint32_t off = 0; int result = 0;
    while (off < size) {
        size_t n = fread(b + 4, 1, sizeof(b) - 4, f);
        if (!n) { result = -1; break; }
        gu_put32(b, off);
        if (gu_call(c, GU_PUT_DATA, b, (uint32_t)n + 4) < 0) { result = -1; break; }
        off += (uint32_t)n;
    }
    fclose(f); erase(b,sizeof b);
    if (!result && gu_call(c, GU_PUT_END, NULL, 0) < 0) result = -1;
    if (result) gu_call(c, GU_ABORT, NULL, 0);
    else fprintf(stderr, "Uploaded and verified %u bytes to %s\n", size, remote);
    return result;
}
int gu_download(struct gu_client *c, const char *remote, const char *local)
{
    if (strlen(remote) > 120) return -1;
    char tmp[1024];
    if (snprintf(tmp, sizeof(tmp), "%s.part", local) >= (int)sizeof(tmp)) return -1;
    FILE *f = fopen(tmp, "wx"); if (!f) { perror(tmp); return -1; }
    uint8_t b[GU_PAYLOAD]; uint32_t off = 0; int result = 0;
    for (;;) {
        gu_put32(b, off); memcpy(b + 4, remote, strlen(remote));
        int n = gu_call(c, GU_GET, b, (uint32_t)strlen(remote) + 4);
        if (n < 0 || fwrite(c->frame + GU_HEADER, 1, (size_t)n, f) != (size_t)n) { result = -1; break; }
        off += (uint32_t)n;
        if ((unsigned)n < GU_PAYLOAD) break;
    }
    if (fclose(f)) result = -1;
    if (!result && rename(tmp, local)) { perror(local); result = -1; }
    if (result) remove(tmp);
    return result;
}
int gu_logs(struct gu_client *c, int once)
{
    unsigned calls = 0;
    do {
        if (usb_cancelled) break;
        int n = gu_call(c, GU_LOG_READ, NULL, 0);
        if (n < 4) return -1;
        const uint8_t *b = c->frame + GU_HEADER;
        uint32_t dropped = gu_get32(b);
        if (dropped) fprintf(stderr, "[USB log: %u bytes dropped]\n", dropped);
        fwrite(b + 4, 1, (size_t)n - 4, stdout); fflush(stdout);
        if (once && n == 4) break;
        if (once && ++calls >= 64) break;
        if (n == 4) usleep(20000);
    } while (1);
    return 0;
}
static int image_paths(const char *dir, char *kernel, char *romfs)
{
    return snprintf(kernel, 1024, "%s/kernel.bin", dir) >= 1024 ||
           snprintf(romfs, 1024, "%s/romfs.img", dir) >= 1024 ? -1 : 0;
}
static int check_images(const char *k, const char *r, uint32_t ks, uint32_t rs, uint32_t ramfs_kb)
{
    uint8_t h[32], magic[8]; struct gu_layout layout;
    FILE *f = fopen(k, "rb"); if (!f) return -1;
    size_t n = fread(h, 1, sizeof(h), f); fclose(f);
    f = fopen(r, "rb"); if (!f) return -1;
    size_t m = fread(magic, 1, sizeof(magic), f); fclose(f);
    if (n != sizeof(h) || m != sizeof(magic) || memcmp(magic, "GZRF", 4) ||
        gu_boot_layout(h, ks, rs, ramfs_kb, &layout)) {
        fprintf(stderr, "Invalid boot images or RAM layout\n"); return -1;
    }
    return 0;
}
int gu_reload(struct gu_client *c, const char *dir)
{
    char paths[2][1024]; uint32_t size[2], crc[2];
    struct boot_options options;
    if (boot_options(dir, &options)) return -1;
    if (image_paths(dir, paths[0], paths[1])) return -1;
    for (int i = 0; i < 2; i++) if (file_crc(paths[i], &size[i], &crc[i])) return -1;
    if (check_images(paths[0], paths[1], size[0], size[1], options.ramfs_kb)) return -1;
    uint8_t b[GU_PAYLOAD];
    gu_put32(b, size[0]); gu_put32(b + 4, size[1]); gu_put32(b + 8, options.ramfs_kb);
    gu_put32(b + 12, options.flags);
    gu_put32(b + 16, crc[0]); gu_put32(b + 20, crc[1]);
    if (gu_call(c, GU_BOOT_BEGIN, b, 24) < 0) return -1;
    uint32_t off = 0;
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) goto fail;
        uint32_t left = size[i];
        while (left) {
            size_t n = left < sizeof(b) - 4 ? left : sizeof(b) - 4;
            if (fread(b + 4, 1, n, f) != n) { fclose(f); goto fail; }
            gu_put32(b, off);
            if (gu_call(c, GU_BOOT_DATA, b, (uint32_t)n + 4) < 0) { fclose(f); goto fail; }
            off += (uint32_t)n; left -= (uint32_t)n;
        }
        fclose(f);
        if (!i && off % 4096) {
            uint32_t pad = 4096 - off % 4096;
            while (pad) {
                uint32_t n = pad < sizeof(b) - 4 ? pad : sizeof(b) - 4;
                memset(b + 4, 0, n); gu_put32(b, off);
                if (gu_call(c, GU_BOOT_DATA, b, n + 4) < 0) goto fail;
                off += n; pad -= n;
            }
        }
    }
    if (gu_call(c, GU_BOOT_COMMIT, NULL, 0) < 0) goto fail;
    fprintf(stderr, "Reload accepted; waiting for the new kernel\n"); return 0;
fail:
    gu_call(c, GU_ABORT, NULL, 0); return -1;
}
static int ce_folder(struct pc_link *p, uint64_t need, char folder[512])
{
    const char *roots[] = {"\\Storage Card", "\\Nand Disk"};
    for (unsigned i = 0; i < 2; i++) {
        uint64_t free_bytes;
        if (pc_free_space(p, roots[i], &free_bytes) || free_bytes < need) continue;
        snprintf(folder, 512, "%s\\GlazeUSB", roots[i]);
        return pc_mkdir(p, folder);
    }
    fprintf(stderr, "No CE staging volume has enough free space\n"); return -1;
}
static int ce_test(struct pc_link *p, const char *dir, const char *folder)
{
    char local[1024], remote[600], reply[1024];
    if (snprintf(local, sizeof(local), "%s/UsbProbe.exe", dir) >= (int)sizeof(local) ||
        snprintf(reply, sizeof(reply), "%s/usb-probe-return.txt", dir) >= (int)sizeof(reply)) return -1;
    char nonce[32]; snprintf(nonce, sizeof(nonce), "%llx-%x", (unsigned long long)now(), (unsigned)getpid());
    snprintf(remote, sizeof(remote), "%s\\UsbProbe.exe", folder);
    if (pc_put(p, local, remote) || pc_run(p, remote, nonce)) return -1;
    snprintf(remote, sizeof(remote), "%s\\usb-probe.txt", folder);
    char expected[100]; int size = snprintf(expected, sizeof(expected), "Glaze USB CE execution verified: %s\r\n", nonce);
    if (size < 0 || size >= (int)sizeof(expected)) return -1;
    uint64_t deadline = now() + 3000;
    do {
        if (!pc_get(p, remote, reply)) {
            FILE *f = fopen(reply, "rb");
            if (!f) return -1;
            char b[100]; size_t n = fread(b, 1, sizeof(b), f); fclose(f);
            if (n == (size_t)size && !memcmp(b, expected, n)) {
                fprintf(stderr, "CE USB upload, read-back and executable launch verified\n");
                return 0;
            }
        }
        /* The preceding run's file may still exist until the new process
         * is scheduled. Only this run's nonce proves execution. */
        usleep(10000);
    } while (now() < deadline && !usb_cancelled);
    fprintf(stderr, "CE probe did not produce this run's execution nonce\n");
    return -1;
}
int ce_self_test(struct usb_link *u, const char *dir)
{
    struct pc_link p; char folder[512];
    if (pc_connect(&p, u) || ce_folder(&p, 65536, folder)) return -1;
    return ce_test(&p, dir, folder);
}
int ce_boot(struct usb_link *u, const char *dir)
{
    char kernel[1024], romfs[1024]; uint32_t ks, rs, kc, rc;
    struct boot_options options;
    if (boot_options(dir, &options)) return -1;
    if (image_paths(dir, kernel, romfs) || file_crc(kernel, &ks, &kc) || file_crc(romfs, &rs, &rc) ||
        check_images(kernel, romfs, ks, rs, options.ramfs_kb)) return -1;
    struct pc_link p;
    if (pc_connect(&p, u)) return -1;
    char folder[512], remote[600], local[1024];
    if (ce_folder(&p, (uint64_t)ks + rs + 262144, folder) || ce_test(&p, dir, folder)) return -1;
    const char *files[] = {"GlazeBoot.exe", "kernel.bin", "romfs.img", "glaze.cfg"};
    for (unsigned i = 0; i < 4; i++) {
        snprintf(remote, sizeof(remote), "%s\\%s", folder, files[i]);
        if (snprintf(local, sizeof(local), "%s/%s", dir, files[i]) >= (int)sizeof(local)) return -1;
        if (pc_put(&p, local, remote)) return -1;
    }
    snprintf(remote, sizeof(remote), "%s\\GlazeBoot.exe", folder);
    return pc_run(&p, remote, "--usb-boot");
}
