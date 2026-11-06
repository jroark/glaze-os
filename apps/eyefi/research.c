/* Research commands, read from RESEARCH_SCRIPT (one per line), for finding
 * a network channel through the card (docs/hardware/EYEFI.md). Responses
 * are saved under RESEARCH_DIR, trimmed of trailing zeroes; the console log
 * gets lengths and short hex prefixes. The upload key is only written to a
 * file, never printed.
 *
 *   info N | probe A B           'o' token reads (probe skips 3 and 0xfd)
 *   set N HEX | setstr N TEXT    'O' token writes
 *   raw HEX                      an arbitrary request
 *   scan | list | reboot | direct-start      'g' 'l' 'b' 'S'
 *   wlan on|off
 *   addnet HEXKEY SSID | testnet HEXKEY SSID | delnet SSID
 *   log                          the card log ('o' 7, then 'm' pieces)
 *   uploadkey                    'o' 0xfd to RESEARCH_DIR/uploadkey.bin
 *   forget-key                   remove that file once it has been copied off
 *   drop SRC NAME                copy a Glaze file into /Card/DCIM/100EYEFI
 *   watch SECONDS                print 'o' 11/13/14 whenever they change
 *   sleep MS
 */
#include <glaze/abi.h>
#include <glaze/fs.h>
#include <glaze/sys.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "protocol.h"
#include "research.h"

#define RESEARCH_DIR "/Library/Caches/eyefi"
#define DROP_DIR "/Card/DCIM/100EYEFI"

static uint8_t req[EYEFI_REQUEST_MAX];
static unsigned saved_files;

static unsigned trimmed(const uint8_t *b, unsigned n)
{
    while (n > 1 && !b[n - 1]) --n;
    return n;
}

static int save(const char *name, const uint8_t *data, unsigned len, int append)
{
    char path[FS_PATH_MAX];
    snprintf(path, sizeof(path), RESEARCH_DIR "/%s", name);
    int fd = fs_open(path, FS_O_WRITE | FS_O_CREATE | (append ? FS_O_APPEND : FS_O_TRUNC));
    if (fd < 0) return fd;
    int r = 0;
    for (unsigned at = 0; at < len && r >= 0; at += FS_IO_MAX) {
        unsigned n = len - at < FS_IO_MAX ? len - at : FS_IO_MAX;
        int w = fs_write(fd, data + at, n);
        if (w != (int)n) r = w < 0 ? w : GZ_EIO;
    }
    int c = fs_close(fd);
    return r < 0 ? r : c;
}

static void show(const char *what, const uint8_t *b, unsigned n)
{
    printf("eyefi: %s: %u bytes:", what, n);
    for (unsigned i = 0; i < n && i < 48; ++i) printf(" %02x", b[i]);
    printf(n > 48 ? " ...\n" : "\n");
}

/* Send, then save the trimmed response as NNN-label.bin. */
static int exchange(const struct eyefi_io *io, uint8_t *buf, int len,
                    const char *label, int quiet)
{
    if (len < 0) return len;
    int r = eyefi_request(io, req, (unsigned)len, buf);
    if (r < 0) { printf("eyefi: %s failed (%d)\n", label, r); return r; }
    unsigned n = trimmed(buf, EYEFI_SIZE);
    char name[48];
    snprintf(name, sizeof(name), "%03u-%s.bin", saved_files++, label);
    if ((r = save(name, buf, n, 0)) < 0) {
        printf("eyefi: saving %s failed (%d)\n", name, r);
        return r;
    }
    if (!quiet) show(name, buf, n);
    printf("eyefi: saved " RESEARCH_DIR "/%s\n", name);
    return 0;
}

static int hex_value(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int parse_hex(const char *s, uint8_t *out, unsigned max)
{
    unsigned n = 0;
    while (*s) {
        int hi = hex_value(s[0]), lo = hi < 0 ? -1 : hex_value(s[1]);
        if (lo < 0 || n == max) return GZ_EINVAL;
        out[n++] = (uint8_t)(hi << 4 | lo);
        s += 2;
    }
    return (int)n;
}

static int number(const char *s, unsigned max, unsigned *out)
{
    char *end;
    if (!s || !*s) return GZ_EINVAL;
    unsigned long v = strtoul(s, &end, 0);
    if (*end || v > max) return GZ_EINVAL;
    *out = (unsigned)v;
    return 0;
}

/* Splits off the next space-separated word; *rest gets the remainder. */
static char *word(char **rest)
{
    char *s = *rest;
    while (*s == ' ') ++s;
    if (!*s) { *rest = s; return NULL; }
    char *e = s;
    while (*e && *e != ' ') ++e;
    if (*e) *e++ = 0;
    *rest = e;
    return s;
}

static int probe(const struct eyefi_io *io, uint8_t *buf, unsigned from, unsigned to)
{
    char name[32];
    snprintf(name, sizeof(name), "probe-%02x-%02x.bin", from, to);
    int r = save(name, NULL, 0, 0);
    if (r < 0) return r;
    printf("eyefi: saved " RESEARCH_DIR "/%s\n", name);
    for (unsigned t = from; t <= to; ++t) {
        if (t == 3 || t == 0xfd) continue;      /* card and upload keys */
        eyefi_req_info(req, t);
        r = eyefi_request(io, req, 2, buf);
        if (r == GZ_ETIMEDOUT) { printf("eyefi: o %02x: no response\n", t); continue; }
        if (r < 0) { printf("eyefi: o %02x failed (%d)\n", t, r); return r; }
        /* Record: token, little-endian 16-bit length, then the bytes. */
        unsigned n = trimmed(buf, 1024);
        uint8_t head[3] = {(uint8_t)t, (uint8_t)n, (uint8_t)(n >> 8)};
        if ((r = save(name, head, 3, 1)) < 0 || (r = save(name, buf, n, 1)) < 0) return r;
        char what[8];
        snprintf(what, sizeof(what), "o %02x", t);
        /* The card leaves earlier replies' bytes in RSPM past the length
         * byte, so the console shows only the reply itself. */
        if (buf[0]) show(what, buf, buf[0] + 1u);
    }
    return 0;
}

static int fetch_log(const struct eyefi_io *io, uint8_t *buf)
{
    eyefi_req_info(req, 7);
    int r = eyefi_request(io, req, 2, buf);
    if (r < 0) return r;
    if (buf[0] != 4) { show("log length (unexpected)", buf, trimmed(buf, 16)); return GZ_ECORRUPT; }
    uint32_t total = (uint32_t)buf[1] << 24 | (uint32_t)buf[2] << 16 |
                     (uint32_t)buf[3] << 8 | buf[4];
    printf("eyefi: log length %lu\n", (unsigned long)total);
    if (total > 1024u * 1024u) return GZ_ECORRUPT;
    for (uint32_t at = 0; at < total; at += EYEFI_SIZE) {
        eyefi_req_log(req, at);
        if ((r = eyefi_request(io, req, 5, buf)) < 0) return r;
        char name[32];
        snprintf(name, sizeof(name), "log-%06lx.bin", (unsigned long)at);
        if ((r = save(name, buf, EYEFI_SIZE, 0)) < 0) return r;
        printf("eyefi: saved " RESEARCH_DIR "/%s\n", name);
    }
    return 0;
}

static int status_watch(const struct eyefi_io *io, uint8_t *buf, unsigned seconds)
{
    static const unsigned tokens[] = {11, 13, 14};
    static uint8_t last[3][64];
    memset(last, 0xff, sizeof(last));
    for (unsigned s = 0; s <= seconds; ++s) {
        for (unsigned i = 0; i < 3; ++i) {
            eyefi_req_info(req, tokens[i]);
            int r = eyefi_request(io, req, 2, buf);
            if (r < 0) { printf("eyefi: watch o %u failed (%d)\n", tokens[i], r); return r; }
            if (memcmp(last[i], buf, sizeof(last[i]))) {
                memcpy(last[i], buf, sizeof(last[i]));
                char what[24];
                snprintf(what, sizeof(what), "t+%us o %u", s, tokens[i]);
                show(what, buf, trimmed(buf, 64));
            }
        }
        gz_sleep_ms(1000);
    }
    return 0;
}

static int drop(const char *src, const char *name, uint8_t *buf)
{
    char path[FS_PATH_MAX];
    if (strchr(name, '/') || snprintf(path, sizeof(path), DROP_DIR "/%s", name) >= (int)sizeof(path))
        return GZ_EINVAL;
    int r = fs_mkdirs(DROP_DIR);
    if (r < 0 && r != GZ_EEXIST) return r;
    int in = fs_open(src, FS_O_READ);
    if (in < 0) return in;
    int out = fs_open(path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
    if (out < 0) { fs_close(in); return out; }
    unsigned long total = 0;
    for (;;) {
        int n = fs_read(in, buf, FS_IO_MAX);
        if (n <= 0) { r = n; break; }
        int w = fs_write(out, buf, (uint32_t)n);
        if (w != n) { r = w < 0 ? w : GZ_EIO; break; }
        total += (unsigned)n;
    }
    int c = fs_close(out);
    fs_close(in);
    if (r >= 0) r = c;
    if (r >= 0) printf("eyefi: dropped %lu bytes as %s (closed at %lu ms)\n",
                       total, path, (unsigned long)gz_time_ms());
    return r;
}

static int command(const struct eyefi_io *io, uint8_t *buf, char *line)
{
    char *rest = line, *cmd = word(&rest), *a = NULL, *b = NULL;
    unsigned x, y;
    uint8_t bytes[EYEFI_REQUEST_MAX];
    int n;
    if (!cmd || cmd[0] == '#') return 0;
    int secret = !strcmp(cmd, "addnet") || !strcmp(cmd, "testnet");
    printf("eyefi: > %s %s\n", cmd, secret ? "(key elided)" : rest);
    if (!strcmp(cmd, "info")) {
        if (number(word(&rest), 255, &x) < 0) return GZ_EINVAL;
        if (x == 3 || x == 0xfd) return GZ_EINVAL;
        char label[12]; snprintf(label, sizeof(label), "o%02x", x);
        return exchange(io, buf, eyefi_req_info(req, x), label, 0);
    }
    if (!strcmp(cmd, "probe")) {
        if (number(word(&rest), 255, &x) < 0 || number(word(&rest), 255, &y) < 0 || y < x)
            return GZ_EINVAL;
        return probe(io, buf, x, y);
    }
    if (!strcmp(cmd, "set") || !strcmp(cmd, "setstr")) {
        if (number(word(&rest), 255, &x) < 0) return GZ_EINVAL;
        if (cmd[3]) { n = (int)strlen(rest); if (n > 255) return GZ_EINVAL; memcpy(bytes, rest, (size_t)n); }
        else if ((n = parse_hex((a = word(&rest)) ? a : "", bytes, 255)) < 0) return n;
        char label[12]; snprintf(label, sizeof(label), "O%02x", x);
        return exchange(io, buf, eyefi_req_config(req, x, bytes, (unsigned)n), label, 0);
    }
    if (!strcmp(cmd, "raw")) {
        if (!(a = word(&rest)) || (n = parse_hex(a, req, sizeof(req))) <= 0) return GZ_EINVAL;
        return exchange(io, buf, n, "raw", 0);
    }
    if (!strcmp(cmd, "scan"))   return exchange(io, buf, eyefi_req_noarg(req, 'g'), "scan", 0);
    if (!strcmp(cmd, "list"))   return exchange(io, buf, eyefi_req_noarg(req, 'l'), "list", 0);
    if (!strcmp(cmd, "reboot")) return exchange(io, buf, eyefi_req_noarg(req, 'b'), "reboot", 0);
    if (!strcmp(cmd, "direct-start")) return exchange(io, buf, eyefi_req_noarg(req, 'S'), "direct", 0);
    if (!strcmp(cmd, "wlan")) {
        if (!(a = word(&rest)) || (strcmp(a, "on") && strcmp(a, "off"))) return GZ_EINVAL;
        return exchange(io, buf, eyefi_req_wlan(req, !strcmp(a, "off")), "wlan", 0);
    }
    if (!strcmp(cmd, "addnet") || !strcmp(cmd, "testnet")) {
        if (!(a = word(&rest)) || (n = parse_hex(a, bytes, EYEFI_KEY_MAX)) < 0) return GZ_EINVAL;
        while (*rest == ' ') ++rest;
        return exchange(io, buf, eyefi_req_network(req, cmd[0], rest, bytes, (unsigned)n), cmd, 0);
    }
    if (!strcmp(cmd, "delnet")) {
        while (*rest == ' ') ++rest;
        return exchange(io, buf, eyefi_req_network(req, 'd', rest, NULL, 0), "delnet", 0);
    }
    if (!strcmp(cmd, "log")) return fetch_log(io, buf);
    if (!strcmp(cmd, "uploadkey")) {
        n = eyefi_request(io, (const uint8_t[]){'o', 0xfd}, 2, buf);
        if (n < 0) return n;
        if (!buf[0]) {
            printf("eyefi: upload key reply has length 0\n");
            memset(buf, 0, EYEFI_SIZE);
            return GZ_ECORRUPT;
        }
        n = save("uploadkey.bin", buf + 1, buf[0], 0);
        if (n >= 0) printf("eyefi: upload key (%u bytes) saved to " RESEARCH_DIR "/uploadkey.bin\n", buf[0]);
        memset(buf, 0, EYEFI_SIZE);
        return n;
    }
    if (!strcmp(cmd, "forget-key")) {
        n = fs_remove(RESEARCH_DIR "/uploadkey.bin");
        printf("eyefi: upload key file removed (%d)\n", n);
        return n == GZ_ENOENT ? 0 : n;
    }
    if (!strcmp(cmd, "drop")) {
        if (!(a = word(&rest)) || !(b = word(&rest))) return GZ_EINVAL;
        return drop(a, b, buf);
    }
    if (!strcmp(cmd, "watch")) {
        if (number(word(&rest), 3600, &x) < 0) return GZ_EINVAL;
        return status_watch(io, buf, x);
    }
    if (!strcmp(cmd, "sleep")) {
        if (number(word(&rest), 600000, &x) < 0) return GZ_EINVAL;
        gz_sleep_ms(x);
        return 0;
    }
    return GZ_EINVAL;
}

int eyefi_research(const struct eyefi_io *io, uint8_t *buf, char *script)
{
    int r = fs_mkdirs(RESEARCH_DIR);
    if (r < 0 && r != GZ_EEXIST) { printf("eyefi: cannot create " RESEARCH_DIR " (%d)\n", r); return r; }
    saved_files = 0;
    for (char *line = script; line && *line; ) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[len - 1] = 0;
        if ((r = command(io, buf, line)) < 0) {
            printf("eyefi: command failed (%d); stopping\n", r);
            return r;
        }
        line = next;
    }
    printf("eyefi: research script complete\n");
    return 0;
}
