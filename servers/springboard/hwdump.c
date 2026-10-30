/*
 * Saving the kernel's register snapshots (kernel/hwdump.c, HWDUMP=1
 * builds) to the card. A cold boot may have neither serial nor USB, so
 * the dump goes to /Card/HWDUMPnn.BIN, never replacing an earlier one,
 * and notifications show the result. A cold boot may not be able to write
 * the card either, so they also carry the values that matter most: the
 * clock gates and the socket's bus setup. Ordinary kernels report no dump
 * and this does nothing.
 */

#include "sb_internal.h"

#include <glaze/fs.h>
#include <glaze/notify.h>
#include <glaze/sys.h>
#include <glaze/thread.h>
#include <stdio.h>
#include <stdlib.h>

#define CARD_WAIT_MS    (90 * 1000)
#define CREATE_TRIES    30

static void post(uint32_t id, const char *title, const char *body)
{
    struct gz_datetime dt;
    uint32_t now = 0;

    if (!gz_rtc_get(&dt))
        now = gz_dt_to_secs(&dt);
    sb_note_post_for("bin/settings", 0x48574450u + id, now, title, body, 0);
}

static char giu[24];     /* power-on GPIO direction/data, for every report */

static void report(const char *body)
{
    char line[96];

    snprintf(line, sizeof(line), "%s %s", body, giu);
    post(0, "Hardware dump", line);
}

/* A register's value in a slot (kernel/hwdump.c's format), or -1 when it
 * wasn't read. */
static int64_t lookup(const uint32_t *w, uint32_t n, uint32_t slot, uint32_t pa)
{
    uint32_t i = 4, cur = ~0u;

    while (i + 1 < n) {
        if (w[i] == 0x544F4C53u) {      /* "SLOT" */
            cur = w[i + 1];
            i += 5;
            continue;
        }
        uint32_t base = w[i], desc = w[i + 1];
        uint32_t count = desc & 0x80000000u ? 0 : desc & 0xFFFF;
        uint32_t stride = ((desc >> 16) & 0x7FFF) == 2 ? 2 : 4;
        if (cur == slot && pa >= base && pa < base + count * stride && (pa - base) % stride == 0) {
            uint32_t at = i + 2 + (pa - base) / stride;
            return at < n ? (int64_t)w[at] : -1;
        }
        i += 2 + count;
    }
    return -1;
}

/* The 16 gates 0A001100..113C as a mask, bit n for 1100 + 4n. */
static uint32_t gates(const uint32_t *w, uint32_t n, uint32_t slot)
{
    uint32_t mask = 0;

    for (uint32_t g = 0; g < 16; g++)
        if (lookup(w, n, slot, 0x0A001100u + 4 * g) > 0)
            mask |= 1u << g;
    return mask;
}

/* The lock screen shows about 30 characters of a body, so each value
 * group gets its own notification. */
static void summarize(const uint32_t *w, uint32_t n)
{
    char body[96];

    snprintf(body, sizeof(body), "entry %04x setup %04x PIU %s",
             (unsigned)gates(w, n, 0), (unsigned)gates(w, n, 1),
             lookup(w, n, 1, 0x0A000300u) < 0 ? "-" : "r");
    post(1, "Gates 1100-113C", body);
    snprintf(body, sizeof(body), "A020 %x A024 %x A034 %x L%x M%x",
             (unsigned)lookup(w, n, 1, 0x0A00A020u), (unsigned)lookup(w, n, 1, 0x0A00A024u),
             (unsigned)lookup(w, n, 1, 0x0A00A034u), (unsigned)lookup(w, n, 1, 0x0A00868Cu),
             (unsigned)lookup(w, n, 1, 0x0A008690u));
    post(2, "Socket, serial", body);
}

static void worker(void *arg)
{
    int size = gz_hwdump(GZ_HWDUMP_SNAPSHOTS, NULL, 0);
    uint8_t *buf = size > 0 ? malloc((uint32_t)size) : NULL;
    char path[32], msg[64];
    struct fs_info info;
    int fd = -1, n;

    if (!buf || gz_hwdump(GZ_HWDUMP_SNAPSHOTS, buf, (uint32_t)size) != size) {
        report("Couldn't read the snapshot");
        return;
    }
    snprintf(giu, sizeof(giu), "G%x/%x",
             (unsigned)lookup((const uint32_t *)buf, (uint32_t)size / 4, 0, 0x0F000140u),
             (unsigned)lookup((const uint32_t *)buf, (uint32_t)size / 4, 0, 0x0F000144u));
    summarize((const uint32_t *)buf, (uint32_t)size / 4);
    /* Also on the flash volume (a RAM disk in a recovery boot), which USB
     * can fetch when the card can't be written. */
    fs_mkdirs("/Library/Caches");
    int r = fs_write_file("/Library/Caches/HWDUMP.BIN", buf, (uint32_t)size);
    printf("hwdump: /Library/Caches/HWDUMP.BIN (%d)\n", r);
    for (int t = 0; fs_statvfs("/Card", &info) < 0; t += 1000) {
        if (t >= CARD_WAIT_MS) {
            report("No card to save it on");
            return;
        }
        gz_sleep_ms(1000);
    }
    /* A socket that drops the card for a moment remounts it: retry for
     * CREATE_TRIES seconds, then report the socket's history. */
    int tries = 0;
    do {
        if (tries)
            gz_sleep_ms(1000);
        fd = -1;
        for (n = 0; n < 100 && fd < 0; n++) {
            snprintf(path, sizeof(path), "/Card/HWDUMP%02d.BIN", n);
            fd = fs_open(path, FS_O_WRITE | FS_O_CREATE | FS_O_EXCL);
            if (fd < 0 && fd != GZ_EEXIST)
                break;
        }
    } while (fd < 0 && ++tries < CREATE_TRIES);
    uint32_t cf[8] = { 0 };
    gz_hwdump(GZ_HWDUMP_CF, cf, sizeof(cf));
    /* Status now/at cold init, cold inserts, inserts, probes, removals. */
    snprintf(msg, sizeof(msg), "st %x/%x c%u i%u p%u r%u %s g%u",
             (unsigned)cf[0], (unsigned)cf[1], (unsigned)cf[2], (unsigned)cf[3],
             (unsigned)cf[4], (unsigned)cf[5], cf[6] ? "on" : "off", (unsigned)cf[7]);
    post(3, "Card socket", msg);
    printf("hwdump: card socket %s\n", msg);
    if (fd < 0) {
        snprintf(msg, sizeof(msg), "Create %d x%d", fd, tries);
        printf("hwdump: %s\n", msg);
        report(msg);
        return;
    }
    int w = fs_write(fd, buf, (uint32_t)size);
    int c = fs_close(fd);
    printf("hwdump: %s, %d bytes (%d, %d)\n", path, size, w, c);
    if (w == size && c >= 0)
        snprintf(msg, sizeof(msg), "Saved %s", path + 6);
    else
        snprintf(msg, sizeof(msg), "Write %d close %d %s", w, c, fs_strerror(w < 0 ? w : c));
    report(msg);
}

void hwdump_start(void)
{
    if (gz_hwdump(GZ_HWDUMP_SNAPSHOTS, NULL, 0) > 0)
        gz_thread_create(worker, NULL);
}
