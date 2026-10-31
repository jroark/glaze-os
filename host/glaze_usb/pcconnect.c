#include "pcconnect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void pc_checksums(uint8_t *p, unsigned n)
{
    unsigned sum = 0;
    for (unsigned i = 10; i < n; i++) sum += p[i];
    pc_put16(p + 6, sum);
    sum = 0;
    for (unsigned i = 0; i < 8; i++) sum += p[i];
    pc_put16(p + 8, sum);
}
int pc_valid(const uint8_t *p, unsigned n)
{
    if (n < 10 || n > PC_MAX || pc_be16(p + 2) != n) return 0;
    unsigned h = 0, d = 0;
    for (unsigned i = 0; i < 8; i++) h += p[i];
    for (unsigned i = 10; i < n; i++) d += p[i];
    return (h & 65535) == pc_be16(p + 8) && (d & 65535) == pc_be16(p + 6);
}
/* Byte length (BE16), then UTF-16BE including NUL. */
int pc_path(uint8_t *out, unsigned cap, const char *path)
{
    unsigned n = (unsigned)strlen(path);
    if (n > 250 || cap < 4 + n * 2) return -1;
    pc_put16(out, (n + 1) * 2);
    for (unsigned i = 0; i <= n; i++) {
        if ((unsigned char)path[i] >= 128) return -1;
        out[2 + i * 2] = 0; out[3 + i * 2] = (uint8_t)(path[i] == '/' ? '\\' : path[i]);
    }
    return (int)(4 + n * 2);
}
static uint64_t ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (unsigned)t.tv_nsec / 1000000;
}
static int next_byte(struct pc_link *p, unsigned timeout)
{
    if (p->input_pos == p->input_len) {
        int n = usb_read(p->usb, p->input, sizeof(p->input), timeout);
        if (n <= 0) return n ? n : LIBUSB_ERROR_TIMEOUT;
        p->input_pos = 0; p->input_len = (unsigned)n;
    }
    return p->input[p->input_pos++];
}
static int receive(struct pc_link *p, unsigned op, unsigned timeout)
{
    unsigned used = 0, want = 10; uint64_t end = ms() + timeout, retry = ms() + 500;
    while (ms() < end && !usb_cancelled) {
        if (op == 0x032d && !used && ms() >= retry) {
            if (!++p->seq) ++p->seq;
            uint8_t query[10] = {0x10, p->seq, 0, 10, 3, 0x2d};
            pc_checksums(query, sizeof(query));
            p->request_data_sum = 0; p->request_header_sum = pc_be16(query + 8);
            int r = usb_write(p->usb, query, sizeof(query), 500);
            if (r) return r;
            retry = ms() + 500;
        }
        int b = next_byte(p, 200);
        if (b == LIBUSB_ERROR_TIMEOUT) continue;
        if (b < 0) return b;
        if (!used && b != 0x11) continue; /* wake train / AT chatter */
        p->rx[used++] = (uint8_t)b;
        if (used == 10) {
            want = pc_be16(p->rx + 2);
            if (want < 10) return LIBUSB_ERROR_IO;
        }
        if (used == want) {
            if (p->rx[1] != p->seq || pc_be16(p->rx + 4) != op) {
                used = 0; want = 10; continue;
            }
            /* Some stock PCLASRV builds leave the request's checksums in
             * their reply (the SDK's receiver does not validate them).
             * Accept only that exact echo, or a fully checksummed reply. */
            if (!pc_valid(p->rx, want) &&
                (pc_be16(p->rx + 6) != p->request_data_sum || pc_be16(p->rx + 8) != p->request_header_sum)) return LIBUSB_ERROR_IO;
            if (want < 18) return LIBUSB_ERROR_IO;
            p->result = pc_be32(p->rx + 10); p->error = pc_be32(p->rx + 14);
            return (int)want;
        }
    }
    return LIBUSB_ERROR_TIMEOUT;
}
int pc_call(struct pc_link *p, unsigned op, const void *data, unsigned n)
{
    p->result = p->error = 0;
    if (n > PC_MAX - 10) return LIBUSB_ERROR_INVALID_PARAM;
    uint8_t *frame = malloc(n + 10);
    if (!frame) return LIBUSB_ERROR_NO_MEM;
    frame[0] = 0x10; frame[1] = ++p->seq;
    pc_put16(frame + 2, n + 10); pc_put16(frame + 4, op);
    if (n) memcpy(frame + 10, data, n);
    pc_checksums(frame, n + 10);
    p->request_data_sum = pc_be16(frame + 6); p->request_header_sum = pc_be16(frame + 8);
    int r = usb_write(p->usb, frame, n + 10, 5000); free(frame);
    if (!r) r = receive(p, op, 10000);
    if (r < 0) fprintf(stderr, "PC Connect %04x: %s\n", op, libusb_error_name(r));
    return r;
}
int pc_reboot(struct pc_link *p)
{
    /* Casio SDK WilCLBSoftReset: opcode 032a, no arguments and no reply.
     * Send once: a reboot is a mutation and must never be retried blindly. */
    uint8_t frame[10] = {0x10, ++p->seq, 0, 10, 3, 0x2a};
    pc_checksums(frame, sizeof(frame));
    int r = usb_write(p->usb, frame, sizeof(frame), 5000);
    if (!r) fprintf(stderr, "Windows CE reboot requested\n");
    else fprintf(stderr, "CE reboot: %s\n", libusb_error_name(r));
    return r;
}
int pc_connect(struct pc_link *p, struct usb_link *u)
{
    memset(p, 0, sizeof(*p)); p->usb = u;
    int r = usb_lines(u, 0);
    if (r < 0) return r;
    /* CE's link worker notices a lost carrier on its own timeout. Give an
     * abandoned session time to close before asserting the next carrier. */
    usleep(4500000);
    r = usb_lines(u, 3);
    if (r < 0) return r;
    /* Stock CE can route a newly opened dock through its modem detector
     * before AtPcCnct starts. These attention bytes precede synchronization. */
    static const uint8_t attention[] = {'A', 'T'};
    for (unsigned i = 0; i < sizeof(attention); i++) {
        r = usb_write(u, attention + i, 1, 500);
        if (r < 0) return r;
        usleep(10000);
    }
    /* The first OUT bytes would be discarded before AtPcCnct opens COM.
     * Its wake train proves the port has opened. */
    uint8_t wake[64], sync = 0x55;
    fprintf(stderr, "Waiting for CE PC Connect (up to 60 seconds)...\n");
    uint64_t deadline = ms() + 60000, next_sync = 0;
    unsigned heard = 0;
    while (ms() < deadline && !usb_cancelled) {
        if (ms() >= next_sync) {
            for (unsigned i = 0; i < 2; i++) {
                r = usb_write(u, &sync, 1, 500);
                if (r < 0) return r;
                usleep(10000);
            }
            next_sync = ms() + 500;
        }
        r = usb_read(u, wake, sizeof(wake), 30);
        if (r < 0 && r != LIBUSB_ERROR_TIMEOUT) return r;
        if (r > 0 && !heard) {
            /* Initial OUT may precede the CE port open. Answer its first
             * wake immediately, rather than waiting for the retry timer. */
            heard = 1;
            for (unsigned i = 0; i < 2; i++) {
                r = usb_write(u, &sync, 1, 500);
                if (r < 0) return r;
            }
            continue;
        }
        /* CE queues a train of wake bytes before switching to framed
         * requests. Drain it completely, as the Windows driver does. */
        if (heard && r == LIBUSB_ERROR_TIMEOUT) break;
    }
    if (!heard || ms() >= deadline || usb_cancelled) return LIBUSB_ERROR_TIMEOUT;
    /* This query has no side effects. DTR starts AtPcCnct on stock CE. */
    uint8_t query[10] = {0x10, 1, 0, 10, 3, 0x2d};
    p->seq = 1; pc_checksums(query, sizeof(query));
    p->request_data_sum = 0; p->request_header_sum = pc_be16(query + 8);
    r = usb_write(u, query, sizeof(query), 500);
    if (!r) r = receive(p, 0x032d, 60000);
    if (r >= 0) puts("runtime=Windows CE (Casio PC Connect)");
    return r < 0 ? r : 0;
}

static int success(struct pc_link *p, int n)
{
    if (n < 0) return -1;
    if (!p->result) {
        fprintf(stderr, "CE error %u\n", p->error); return -1;
    }
    return 0;
}
static int handle_close(struct pc_link *p, uint32_t h)
{
    uint8_t b[4]; pc_put32(b, h);
    return success(p, pc_call(p, 0x0309, b, 4));
}
static int handle_open_mode(struct pc_link *p, const char *path, int write, uint32_t *h, int quiet_missing)
{
    uint8_t b[600]; int n = pc_path(b, sizeof(b) - 16, path);
    if (n < 0) return -1;
    pc_put32(b + n, write ? 0xc0000000u : 0x80000000u); /* access */
    pc_put32(b + n + 4, 0); /* share */
    pc_put32(b + n + 8, write ? 1 : 3); /* CREATE_NEW / OPEN_EXISTING */
    pc_put32(b + n + 12, 0x80); /* FILE_ATTRIBUTE_NORMAL */
    int r = pc_call(p, 0x030c, b, (unsigned)n + 16);
    if (r < 0 || p->result == ~0u) {
        if (!quiet_missing || (p->error != 2 && p->error != 3))
            fprintf(stderr, "CE open %s: error %u\n", path, p->error);
        return -1;
    }
    *h = p->result; return 0;
}
static int handle_open(struct pc_link *p, const char *path, int write, uint32_t *h)
{
    return handle_open_mode(p, path, write, h, 0);
}
int pc_file_matches(struct pc_link *p, const char *local, const char *remote)
{
    FILE *f = fopen(local, "rb");
    if (!f) { perror(local); return -1; }
    uint32_t h;
    if (handle_open_mode(p, remote, 0, &h, 1)) {
        fclose(f);
        return p->error == 2 || p->error == 3 ? 0 : -1;
    }
    int result = -1;
    uint8_t request[8], expected[4096];
    for (;;) {
        size_t n = fread(expected, 1, sizeof(expected), f);
        if (ferror(f)) break;
        pc_put32(request, h); pc_put32(request + 4, sizeof(expected));
        int r = pc_call(p, 0x0319, request, sizeof(request));
        if (r < 22 || !p->result) break;
        unsigned got = pc_be32(p->rx + 18);
        if (got > sizeof(expected) || got > (unsigned)r - 22) break;
        if (got != n || memcmp(expected, p->rx + 22, n)) { result = 0; break; }
        if (n < sizeof(expected)) { result = 1; break; }
    }
    fclose(f);
    if (handle_close(p, h)) result = -1;
    return result;
}
int pc_mkdir(struct pc_link *p, const char *path)
{
    uint8_t b[600]; int n = pc_path(b, sizeof(b), path);
    if (n < 0) return -1;
    int r = pc_call(p, 0x030b, b, (unsigned)n);
    if (r >= 0 && (p->result || p->error == 183)) return 0;
    return -1;
}
int pc_free_space(struct pc_link *p, const char *path, uint64_t *free_bytes)
{
    uint8_t b[600]; pc_put32(b, 1);
    int n = pc_path(b + 4, sizeof(b) - 4, path);
    if (n < 0) return -1;
    int r = pc_call(p, 0x034d, b, (unsigned)n + 4);
    if (r < 42 || !p->result) return -1;
    /* LARGE_INTEGER is serialized LowPart then HighPart, each BE32. */
    *free_bytes = pc_be32(p->rx + 18) | (uint64_t)pc_be32(p->rx + 22) << 32;
    return 0;
}
int pc_get(struct pc_link *p, const char *remote, const char *local)
{
    uint32_t h;
    if (handle_open(p, remote, 0, &h)) return -1;
    /* Never truncate an existing host file on a failed transfer. */
    char tmp[1024];
    if (snprintf(tmp, sizeof(tmp), "%s.part", local) >= (int)sizeof(tmp)) { handle_close(p, h); return -1; }
    FILE *f = fopen(tmp, "wx");
    if (!f) { perror(tmp); handle_close(p, h); return -1; }
    int result = -1;
    for (;;) {
        uint8_t b[8]; pc_put32(b, h); pc_put32(b + 4, 4096);
        int r = pc_call(p, 0x0319, b, 8);
        if (r < 22 || !p->result) break;
        unsigned got = pc_be32(p->rx + 18);
        if (got > 4096 || got > (unsigned)r - 22) break;
        if (fwrite(p->rx + 22, 1, got, f) != got) break;
        if (got < 4096) { result = 0; break; }
    }
    if (fclose(f)) result = -1;
    if (handle_close(p, h)) result = -1;
    if (!result && rename(tmp, local)) { perror(local); result = -1; }
    if (result) remove(tmp);
    return result;
}
static int file_move(struct pc_link *p, const char *from, const char *to)
{
    p->result = p->error = 0;
    uint8_t b[1100];
    int n = pc_path(b, sizeof(b), from);
    int m = n < 0 ? -1 : pc_path(b + n, sizeof(b) - (unsigned)n, to);
    return m < 0 ? -1 : success(p, pc_call(p, 0x0318, b, (unsigned)(n + m)));
}
int pc_put(struct pc_link *p, const char *local, const char *remote)
{
    char tmp[512];
    if (snprintf(tmp, sizeof(tmp), "%s.usb-%x-%x.part", remote, (unsigned)getpid(), p->seq) >= (int)sizeof(tmp)) return -1;
    FILE *f = fopen(local, "rb"); if (!f) { perror(local); return -1; }
    fprintf(stderr, "CE upload %s -> %s\n", local, remote);
    uint32_t h;
    if (handle_open(p, tmp, 1, &h)) { fclose(f); return -1; }
    uint8_t b[4104]; int result = 0;
    for (;;) {
        size_t n = fread(b + 8, 1, 4096, f);
        if (!n) { if (ferror(f)) result = -1; break; }
        pc_put32(b, h); pc_put32(b + 4, (uint32_t)n);
        int r = pc_call(p, 0x031f, b, (unsigned)n + 8);
        if (r < 22 || !p->result || pc_be32(p->rx + 18) != n) { result = -1; break; }
    }
    fclose(f);
    if (handle_close(p, h)) result = -1;
    /* Read back every byte before publishing the upload. */
    uint32_t verify;
    if (!result && !handle_open(p, tmp, 0, &verify)) {
        f = fopen(local, "rb");
        if (!f) result = -1;
        while (f && !result) {
            size_t n = fread(b + 8, 1, 4096, f);
            pc_put32(b, verify); pc_put32(b + 4, 4096);
            int r = pc_call(p, 0x0319, b, 8);
            if (r < 22 || !p->result || pc_be32(p->rx + 18) != n || (unsigned)r < n + 22 || memcmp(b + 8, p->rx + 22, n)) { result = -1; break; }
            if (n < 4096) { if (ferror(f)) result = -1; break; }
        }
        if (f) fclose(f);
        if (handle_close(p, verify)) result = -1;
    } else result = -1;
    if (!result) {
        if (file_move(p, tmp, remote)) {
            if (p->error != 183 && p->error != 80) result = -1;
            else {
                /* Preserve the previous destination while replacing it. CE's
                 * MoveFile cannot overwrite. Keep any backup if rollback fails. */
                char backup[512];
                if (snprintf(backup, sizeof(backup), "%s.old", tmp) >= (int)sizeof(backup)) return -1;
                if (file_move(p, remote, backup)) result = -1;
                else {
                    if (file_move(p, tmp, remote)) {
                        if (file_move(p, backup, remote))
                            fprintf(stderr, "Previous file retained at %s\n", backup);
                        result = -1;
                    } else {
                        int a = pc_path(b, sizeof(b), backup);
                        if (a > 0) pc_call(p, 0x030e, b, (unsigned)a);
                    }
                }
            }
        }
    }
    if (result) {
        int n = pc_path(b, sizeof(b), tmp);
        if (n > 0) pc_call(p, 0x030e, b, (unsigned)n);
        fprintf(stderr, "Upload failed: %s\n", remote);
    } else fprintf(stderr, "CE verified and published %s\n", remote);
    return result;
}
int pc_run(struct pc_link *p, const char *remote, const char *args)
{
    uint8_t b[1200]; int n = pc_path(b, sizeof(b) - 8, remote);
    if (n < 0) return -1;
    pc_put32(b + n, args && *args ? 1 : 0); n += 4;
    if (args && *args) {
        int m = pc_path(b + n, sizeof(b) - (unsigned)n - 4, args);
        if (m < 0) return -1;
        n += m;
    }
    pc_put32(b + n, 0); n += 4; /* creation flags */
    int r = pc_call(p, 0x030d, b, (unsigned)n);
    if (success(p, r)) return -1;
    if (r >= 34) {
        uint32_t process = pc_be32(p->rx + 18), thread = pc_be32(p->rx + 22);
        uint32_t pid = pc_be32(p->rx + 26);
        handle_close(p, thread); handle_close(p, process);
        printf("CE launched pid=%u\n", pid);
    }
    return 0;
}
