/* Run the installed eyefi package over USB; output is console log.
 * All mailboxes stay open to bind I/O to the same FatFs mount and to hold
 * FatFs's exclusive writer locks on REQM/REQC throughout the exchange. */
#include <glaze/fs.h>
#include <glaze/sys.h>
#include <stdio.h>
#include <string.h>
#include "protocol.h"
#include "research.h"

#define SCRIPT_PATH "/Library/Caches/eyefi-cmd.txt"
#define SCRIPT_DONE "/Library/Caches/eyefi-cmd.done"

static uint8_t buffer[EYEFI_SIZE] __attribute__((aligned(4)));
static char script[FS_IO_MAX];
static int handles[4] = {-1,-1,-1,-1};
static const char *paths[] = {"/Card/EyeFi/reqm", "/Card/EyeFi/reqc",
                             "/Card/EyeFi/rspm", "/Card/EyeFi/rspc"};

/* Established bootstrap: EyeFi/reqm,reqc,rspm,rspc, each 16 KiB of zeroes.
 * Preflight every existing file before creating anything. Exclusive creation
 * preserves existing mailboxes and their sequence numbers on later runs. */
static int initialize_mailboxes(void)
{
    struct fs_stat st;
    unsigned missing = 0;
    int r = fs_stat("/Card/EyeFi", &st);
    if (r != GZ_ENOENT && (r < 0 || st.type != FS_DIR))
        return r < 0 ? r : GZ_EINVAL;
    for (unsigned i = 0; i < 4; ++i) {
        r = fs_stat(paths[i], &st);
        if (r == GZ_ENOENT) missing |= 1u << i;
        else if (r < 0 || st.type != FS_FILE || st.size != EYEFI_SIZE) {
            printf("eyefi: invalid existing mailbox %s (%d); preserved\n", paths[i], r);
            return r < 0 ? r : GZ_ECORRUPT;
        }
    }
    if (!missing) return 0;
    r = fs_mkdir("/Card/EyeFi");
    if (r < 0 && r != GZ_EEXIST) return r;
    memset(buffer, 0, sizeof(buffer));
    for (unsigned i = 0; i < 4; ++i) {
        if (!(missing & (1u << i))) continue;
        int fd = fs_open(paths[i], FS_O_WRITE | FS_O_CREATE | FS_O_EXCL);
        if (fd < 0) return fd;
        r = 0;
        for (unsigned at = 0; at < EYEFI_SIZE; at += FS_IO_MAX) {
            int n = fs_write(fd, buffer + at, FS_IO_MAX);
            if (n != FS_IO_MAX) { r = n < 0 ? n : GZ_EIO; break; }
        }
        int closed = fs_close(fd);
        if (r < 0 || closed < 0) {
            printf("eyefi: initialization incomplete for %s; stopping\n", paths[i]);
            return r < 0 ? r : closed;
        }
        printf("eyefi: initialized %s (16384 bytes)\n", paths[i]);
    }
    return 0;
}

static int transfer(unsigned file, uint8_t *buf, int writing)
{
    int r = fs_seek(handles[file], 0, FS_SEEK_SET);
    if (r < 0) return r;
    /* Sector-aligned whole-sector requests bypass FIL's partial-sector
     * cache in FatFs f_read/f_write. CF PIO completes before the reply. */
    for (unsigned at = 0; at < EYEFI_SIZE; at += FS_IO_MAX) {
        r = writing ? fs_write(handles[file], buf+at, FS_IO_MAX)
                    : fs_read(handles[file], buf+at, FS_IO_MAX);
        if (r != FS_IO_MAX) return r < 0 ? r : GZ_EIO;
    }
    return 0;
}
static int read_mail(void *ctx, unsigned file, uint8_t *b)
{ (void)ctx; return transfer(file, b, 0); }
static int write_mail(void *ctx, unsigned file, const uint8_t *b)
{ (void)ctx; return transfer(file, (uint8_t *)b, 1); }
static void pause_mail(void *ctx, unsigned ms)
{ (void)ctx; gz_sleep_ms(ms); }

int main(void)
{
    static const unsigned queries[] = {1,2,10,11,13};
    static const char *labels[] = {"MAC", "firmware", "WLAN disabled",
                                   "upload pending", "connected SSID"};
    const struct eyefi_io io = {NULL, read_mail, write_mail, pause_mail};
    int result = 1;
    struct fs_stat st;
    int init = initialize_mailboxes();
    if (init < 0) { printf("eyefi: mailbox initialization failed (%d)\n", init); goto done; }
    for (unsigned i = 0; i < 4; ++i) {
        int r = fs_stat(paths[i], &st);
        if (r < 0 || st.type != FS_FILE || st.size != EYEFI_SIZE) {
            printf("eyefi: missing or invalid 16 KiB mailbox %s (%d)\n", paths[i], r);
            goto done;
        }
        handles[i] = fs_open(paths[i], FS_O_READ | (i < 2 ? FS_O_WRITE : 0));
        if (handles[i] < 0) {
            printf("eyefi: cannot open %s (%d)\n", paths[i], handles[i]);
            goto done;
        }
    }
    /* A research script runs once: it is renamed before its first command,
     * so a configuration change is never repeated by a later launch. */
    int n = fs_read_file(SCRIPT_PATH, script, sizeof(script) - 1);
    if (n >= 0) {
        script[n] = 0;
        fs_remove(SCRIPT_DONE);
        int r = fs_rename(SCRIPT_PATH, SCRIPT_DONE);
        if (r < 0) { printf("eyefi: cannot retire %s (%d)\n", SCRIPT_PATH, r); goto done; }
        result = eyefi_research(&io, buffer, script) < 0;
        goto done;
    }
    if (n != GZ_ENOENT) { printf("eyefi: cannot read %s (%d)\n", SCRIPT_PATH, n); goto done; }
    for (unsigned i = 0; i < sizeof(queries)/sizeof(queries[0]); ++i) {
        int n = eyefi_query(&io, queries[i], buffer);
        if (n < 0) {
            printf("eyefi: %s failed (%d); stopping\n", labels[i], n);
            goto done;
        }
        printf("eyefi: %s:", labels[i]);
        for (int j = 1; j <= n; ++j) printf(" %02x", buffer[j]);
        if (queries[i] == 2) {
            printf(" (");
            for (int j = 1; j <= n; ++j)
                printf("%c", buffer[j] >= 32 && buffer[j] < 127 ? buffer[j] : '.');
            printf(")");
        }
        printf("\n");
    }
    printf("eyefi: identity/status complete; no general network interface registered\n");
    result = 0;
done:
    for (unsigned i = 0; i < 4; ++i)
        if (handles[i] >= 0) {
            int r = fs_close(handles[i]);
            if (r < 0) { printf("eyefi: close failed (%d)\n", r); result = 1; }
        }
    return result;
}
