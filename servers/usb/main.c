/* USB development service. All filesystem work runs in userspace. */
#include <glaze/usb.h>
#include <glaze/debug_net.h>
#include "network.h"
#include "ethernet.h"
#include "mirror.h"
#include <glaze/fs.h>
#include <glaze/pkg.h>
#include <glaze/prefs.h>
#include <glaze/sb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(struct gz_msg)==GD_CONFIG_BYTES,"pairing IPC record size");
static struct gu_parser parser;
static uint8_t output[GU_FRAME], payload[GU_PAYLOAD];
static int upload = -1;
static char destination[FS_PATH_MAX], temporary[FS_PATH_MAX];
static uint32_t uploaded, expected, expected_crc, crc;
static uint32_t last_id, last_op, last_crc;
static uint8_t last_reply[GU_PAYLOAD];
static uint32_t last_len;
static int last_status;
static bool session, have_last;
static bool network;
static uint32_t activity;
static uint8_t *screen;
static uint32_t copy_until, report_next;
static unsigned reported = ~0u;
static bool reloading;
static uint32_t script_state;

static int debugger_state(unsigned *flags, unsigned *pid)
{
    unsigned f = session ? GU_DEBUG_CONNECTED | (network ? GU_DEBUG_NETWORK : 0) : 0;
    if (session && (upload >= 0 || screen || reloading || (int32_t)(gz_time_ms() - copy_until) < 0)) f |= GU_DEBUG_COPY;
    if (session && script_state == GU_SCRIPT_RUNNING) f |= GU_DEBUG_SCRIPT;
    if (session && mirror_active()) f |= GU_DEBUG_MIRROR;
    int sb = gz_port_lookup(SB_PORT_NAME);
    if (sb < 0) return sb;
    struct gz_msg m = {{0}}, r;
    m.w[0] = SB_CLIENT_OPEN; m.w[3] = (uint32_t)gz_getpid();
    int err = gz_send(sb, &m, &r);
    if (err || (int)r.w[0] < 0) return err ? err : (int)r.w[0];
    m.w[0] = SB_DEBUG_STATE; m.w[1] = f;
    err = gz_send(sb, &m, &r);
    if (err || (int)r.w[0] < 0) return err ? err : (int)r.w[0];
    reported = f; report_next = gz_time_ms() + 500;
    if (flags) *flags = r.w[1];
    if (pid) *pid = r.w[2];
    return 0;
}
static void copying(void)
{
    copy_until = gz_time_ms() + 350;
    debugger_state(NULL, NULL);
}

static void abort_transfer(void)
{
    if (upload >= 0) { fs_close(upload); fs_remove(temporary); upload = -1; }
    if (screen) { free(screen); screen = NULL; }
    mirror_close();
    reloading = false; copy_until = 0; script_state = GU_SCRIPT_EMPTY;
    gz_usb_ctl(GZ_USB_SCRIPT_CANCEL, NULL, 0);
    gz_usb_ctl(GZ_USB_RELOAD_ABORT, NULL, 0);
}
static void wipe_request(uint32_t op)
{
    if (op != GU_ENTROPY && (op != GU_PUT_DATA || strcmp(destination,PREFS_DIR "/" GD_CONFIG))) return;
    volatile uint8_t *p=parser.bytes;
    for (unsigned i=0;i<sizeof(parser.bytes);i++) p[i]=0;
}
static int send_frame(uint32_t op, uint32_t id, int status, const void *data, uint32_t n)
{
    uint32_t size = gu_encode(output, op, id, status, data, n), pos = 0;
    uint32_t end = gz_time_ms() + 3000;
    while (pos < size) {
        int r = network ? debug_net_write(output + pos,size - pos) :
                          gz_usb_ctl(GZ_USB_WRITE, output + pos, size - pos);
        if (r < 0) return r;
        pos += (uint32_t)r;
        if ((int32_t)(gz_time_ms() - end) >= 0) return GZ_ETIMEDOUT;
        if (!r) gz_sleep_ms(1);
    }
    return 0;
}
static int path_read(char *out, uint32_t cap, const uint8_t *in, uint32_t n)
{
    if (!n || n >= cap || memchr(in, 0, n)) return GZ_EINVAL;
    memcpy(out, in, n); out[n] = 0;
    if (strstr(out, "..")) return GZ_EINVAL;
    return 0;
}
static int launch(const char *path)
{
    char bundle[PKG_BUNDLE_MAX];
    size_t n = strlen(path);
    if (n > 4 && !strcmp(path + n - 4, ".gzp")) {
        int r = pkg_install(path, "", bundle);
        if (r) return r;
        path = bundle;
    }
    /* Use SpringBoard to own the app's surface, lifecycle and foreground. */
    int sb = gz_port_lookup(SB_PORT_NAME);
    if (sb < 0) return sb;
    struct gz_msg m = {{0}}, r;
    m.w[0] = SB_CLIENT_OPEN; m.w[3] = (uint32_t)gz_getpid();
    int err = gz_send(sb, &m, &r);
    if (err) return err;
    if ((int)r.w[0] < 0) return (int)r.w[0];
    struct sb_note_req *req = gz_shm_map((int)r.w[1]);
    if (!req) return GZ_ENOMEM;
    if (strlen(path) >= sizeof(req->path)) { gz_shm_unmap(req); return GZ_EINVAL; }
    strlcpy(req->path, path, sizeof(req->path));
    m.w[0] = SB_DEBUG_LAUNCH;
    err = gz_send(sb, &m, &r);
    gz_shm_unmap(req);
    return err ? err : (int)r.w[0];
}
static int dispatch(uint32_t op, const uint8_t *b, uint32_t n, uint32_t *out)
{
    *out = 0;
    if(mirror_active() && (op==GU_PUT_BEGIN || op==GU_BOOT_BEGIN || op==GU_SCREEN_BEGIN ||
        op==GU_USB_NET_OPEN || (op>=GU_SCRIPT_BEGIN && op<=GU_SCRIPT_START)))return GZ_EBUSY;
    if(op>=GU_MIRROR_NEXT && op<=GU_MIRROR_CLOSE){
        if(upload>=0 || screen || reloading || script_state==GU_SCRIPT_RUNNING)return GZ_EBUSY;
        int rv=mirror_dispatch(op,b,n,payload,out);
        if(!rv)debugger_state(NULL,NULL);
        return rv;
    }
    switch (op) {
    case GU_HELLO: {
        if (n) return GZ_EINVAL;
        abort_transfer();
        struct gz_sysinfo si;
        int r = gz_sysinfo(&si); if (r) return r;
        gu_put32(payload, GU_VERSION);
        gu_put32(payload + 4, GU_CAP_FILES | GU_CAP_RUN | GU_CAP_LOGS | GU_CAP_RELOAD |
                             GU_CAP_SCREEN | GU_CAP_SCRIPT | GU_CAP_STATUS | GU_CAP_REBOOT_CE | GU_CAP_ENTROPY | GU_CAP_NETWORK | GU_CAP_USB_NET | GU_CAP_MIRROR);
        gu_put32(payload + 8, si.free_pages * 4096);
        memcpy(payload + 12, si.build, sizeof(si.build));
        payload[43] = 0; *out = 44; session = true; return 0;
    }
    case GU_NET_ENABLE: {
        if (n!=4 || gu_get32(b)>1) return GZ_EINVAL;
        if (gu_get32(b)) {
            uint8_t c[GD_CONFIG_BYTES+1];
            int size=pref_get(GD_CONFIG,c,sizeof c); memset(c,0,sizeof c);
            if (size!=GD_CONFIG_BYTES) return GZ_EINVAL;
        }
        return pref_put(GD_ENABLED,b,n);
    }
    case GU_USB_NET_OPEN:
        if (network || n || upload >= 0 || reloading || screen) return GZ_EINVAL;
        return usb_ethernet_open(gu_get32(parser.bytes + 8), gu_get32(parser.bytes + 20));
    case GU_ENTROPY: {
        if (n != 32) return GZ_EINVAL;
        uint8_t existing[33];
        int size = pref_get("net.entropy", existing, sizeof(existing));
        volatile uint8_t *wipe = existing;
        for (unsigned i = 0; i < sizeof(existing); i++) wipe[i] = 0;
        if (size < 0 && size != GZ_ENOENT) return size;
        /* Preserve the live ratchet: another debugger connection must not
         * replace a seed while the network service is authenticating. */
        if (size != 32) {
            int r = pref_put("net.entropy", b, n);
            if (r) return r;
            puts("usb: entropy seeded from host CSPRNG");
        }
        gu_put32(payload, size != 32); *out = 4; return 0;
    }
    case GU_PUT_BEGIN: {
        copying();
        if (n < 9 || upload >= 0) return GZ_EINVAL;
        int r = path_read(destination, sizeof(destination) - 6, b + 8, n - 8);
        if (r || destination[0] != '/' || !strncmp(destination, "/NAND", 5) || !strncmp(destination, "/Card", 5)) return GZ_EINVAL;
        /* The default development root is RAM. Explicit external storage
         * is not part of the USB service's initial write interface. */
        expected = gu_get32(b); expected_crc = gu_get32(b + 4);
        if (expected > 8 * 1024 * 1024) return GZ_EFULL;
        char parent[FS_PATH_MAX]; strlcpy(parent, destination, sizeof(parent));
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            *slash = 0; r = fs_mkdirs(parent); if (r) return r;
        }
        snprintf(temporary, sizeof(temporary), "%s.part", destination);
        upload = fs_open(temporary, FS_O_WRITE | FS_O_CREATE | FS_O_EXCL);
        if (upload < 0) return upload;
        uploaded = 0; crc = ~0u; return 0;
    }
    case GU_PUT_DATA: {
        copying();
        if (upload < 0 || n < 5 || gu_get32(b) != uploaded || n - 4 > expected - uploaded) return GZ_EINVAL;
        int r = fs_write(upload, b + 4, n - 4);
        if (r != (int)n - 4) return r < 0 ? r : GZ_EIO;
        crc = gu_crc(crc, b + 4, n - 4); uploaded += n - 4; return 0;
    }
    case GU_PUT_END: {
        copying();
        if (n || upload < 0 || uploaded != expected || ~crc != expected_crc) return GZ_EINVAL;
        int r = fs_close(upload); upload = -1;
        if (!r) r = fs_rename(temporary, destination);
        if (r) fs_remove(temporary);
        return r;
    }
    case GU_GET: {
        copying();
        if (n < 5) return GZ_EINVAL;
        char path[FS_PATH_MAX];
        int r = path_read(path, sizeof(path), b + 4, n - 4); if (r) return r;
        int fd = fs_open(path, FS_O_READ); if (fd < 0) return fd;
        r = fs_seek(fd, (int32_t)gu_get32(b), FS_SEEK_SET);
        if (r >= 0) r = fs_read(fd, payload, sizeof(payload));
        fs_close(fd);
        if (r < 0) return r;
        *out = (uint32_t)r; return 0;
    }
    case GU_RUN: {
        char path[FS_PATH_MAX]; int r = path_read(path, sizeof(path), b, n);
        return r ? r : launch(path);
    }
    case GU_SCREEN_BEGIN: {
        if (n) return GZ_EINVAL;
        if (screen) { free(screen); screen = NULL; }
        screen = malloc(GU_SCREEN_BYTES);
        if (!screen) return GZ_ENOMEM;
        /* Capture the preceding displayed frame, before marking the screenshot
         * transfer busy. The capture itself is atomic with compositor flushes. */
        int r = gz_usb_ctl(GZ_USB_SCREEN_COPY, screen, GU_SCREEN_BYTES);
        if (r) { free(screen); screen = NULL; return r; }
        copying();
        gu_put32(payload, 240); gu_put32(payload + 4, 320);
        gu_put32(payload + 8, 1); gu_put32(payload + 12, GU_SCREEN_BYTES);
        *out = 16; return 0;
    }
    case GU_SCREEN_READ: {
        if (n != 4 || !screen || gu_get32(b) >= GU_SCREEN_BYTES) return GZ_EINVAL;
        uint32_t off = gu_get32(b);
        *out = GU_SCREEN_BYTES - off < sizeof(payload) ? GU_SCREEN_BYTES - off : sizeof(payload);
        memcpy(payload, screen + off, *out); copying(); return 0;
    }
    case GU_SCREEN_END:
        if (n) return GZ_EINVAL;
        free(screen); screen = NULL; return 0;
    case GU_SCRIPT_BEGIN:
        if (upload >= 0 || reloading) return GZ_EINVAL;
        script_state = GU_SCRIPT_LOADING; copying();
        return gz_usb_ctl(GZ_USB_SCRIPT_BEGIN, (void *)b, n);
    case GU_SCRIPT_DATA:
        copying(); return gz_usb_ctl(GZ_USB_SCRIPT_DATA, (void *)b, n);
    case GU_SCRIPT_START: {
        if (n) return GZ_EINVAL;
        int r = gz_usb_ctl(GZ_USB_SCRIPT_STATUS, payload, 16);
        if (r != 16 || gu_get32(payload) != GU_SCRIPT_READY) return r < 0 ? r : GZ_EINVAL;
        script_state = GU_SCRIPT_RUNNING; copy_until = 0;
        r = debugger_state(NULL,NULL); /* wake and paint before starting the clock */
        if (!r) r = gz_usb_ctl(GZ_USB_SCRIPT_START, NULL, 0);
        if (r) { script_state = GU_SCRIPT_READY; debugger_state(NULL,NULL); }
        return r;
    }
    case GU_SCRIPT_STATUS: {
        if (n) return GZ_EINVAL;
        int r = gz_usb_ctl(GZ_USB_SCRIPT_STATUS, payload, 16);
        if (r < 0) return r;
        script_state = gu_get32(payload); *out = (uint32_t)r; return 0;
    }
    case GU_SCRIPT_CANCEL:
        if (n) return GZ_EINVAL;
        script_state = GU_SCRIPT_EMPTY;
        return gz_usb_ctl(GZ_USB_SCRIPT_CANCEL, NULL, 0);
    case GU_DEBUG_STATUS: {
        if (n) return GZ_EINVAL;
        unsigned flags, pid; int r = debugger_state(&flags, &pid);
        if (r) return r;
        gu_put32(payload, flags); gu_put32(payload + 4, pid); *out = 8; return 0;
    }
    case GU_LOG_READ: {
        if (n) return GZ_EINVAL;
        int r = gz_usb_ctl(GZ_USB_LOG_READ, payload, sizeof(payload));
        if (r < 0) return r;
        *out = (uint32_t)r; return 0;
    }
    case GU_BOOT_BEGIN: {
        if (upload >= 0) return GZ_EINVAL;
        int r = gz_usb_ctl(GZ_USB_RELOAD_BEGIN, (void *)b, n);
        if (!r) { reloading = true; copying(); }
        return r;
    }
    case GU_BOOT_DATA:
        return gz_usb_ctl(GZ_USB_RELOAD_DATA, (void *)b, n);
    case GU_BOOT_COMMIT:
        if (n) return GZ_EINVAL;
        { int r=debug_net_handoff(); if (r) return r; }
        return gz_usb_ctl(GZ_USB_RELOAD_PREPARE, NULL, 0);
    case GU_ABORT:
        if (n) return GZ_EINVAL;
        abort_transfer(); return 0;
    case GU_REBOOT_CE:
        if (n) return GZ_EINVAL;
        abort_transfer(); return 0; /* acknowledge before stopping USB */
    default: return GZ_EINVAL;
    }
}
int main(void)
{
    uint8_t b[256]; uint32_t connection = ~0u;
    int pairing=gz_port_create();
    if (pairing<0 || gz_port_register(pairing,GD_PAIR_PORT)<0) return 1;
    uint32_t transport=0; gz_usb_ctl(GZ_USB_DEBUG_TRANSPORT,&transport,4);
    gz_usb_ctl(GZ_USB_NET_CLOSE,NULL,0); /* clean up a crashed predecessor */
    debug_net_restore();
    printf("usb: development service ready\n");
    for (;;) {
        int state = gz_usb_ctl(GZ_USB_STATE, NULL, 0);
        if (state < 0) return 1;
        usb_ethernet_poll((uint32_t)state);
        struct gz_msg request,response={{0}};
        int token=gz_recv(pairing,&request,0);
        if (token>0) {
            /* This process is the sole legitimate writer of pairing records:
             * importing cannot overwrite or race its seed ratchet. */
            abort_transfer(); debug_net_drop(); network=false;
            parser.used=0; session=have_last=false;
            uint32_t off=0;
            int status=pref_put(GD_ENABLED,&off,4);
            if (!status) status=pref_put(GD_CONFIG,&request,GD_CONFIG_BYTES);
            volatile uint8_t *wipe=(uint8_t *)&request;
            for (unsigned j=0;j<sizeof request;j++) wipe[j]=0;
            response.w[0]=(uint32_t)status; gz_reply(token,&response);
            debugger_state(NULL,NULL);
        }
        int net=debug_net_poll(!network && session);
        if ((network && net<0) || (!network && net>0)) {
            abort_transfer(); parser.used=0; session=have_last=false;
            network=net>0; transport=network; gz_usb_ctl(GZ_USB_DEBUG_TRANSPORT,&transport,4); activity=gz_time_ms(); debugger_state(NULL,NULL);
        }
        if (!network && (uint32_t)state != connection) {
            abort_transfer(); parser.used = 0; session = have_last = false;
            debugger_state(NULL,NULL);
            connection = (uint32_t)state;
        }
        if ((session || network) && gz_time_ms() - activity >= 30000) {
            abort_transfer(); parser.used = 0; session = have_last = false;
            if (network) { debug_net_drop(); network=false; }
            debugger_state(NULL,NULL);
        }
        int n = network ? debug_net_read(b,sizeof b) : usb_ethernet_active() ? 0 : gz_usb_ctl(GZ_USB_READ, b, sizeof(b));
        if (network && n<0) {
            abort_transfer(); parser.used=0; session=have_last=false;
            network=false; debugger_state(NULL,NULL);
        }
        for (int i = 0; i < n; i++) {
            if (gu_feed(&parser, b[i]) != 1) continue;
            const uint8_t *f = parser.bytes;
            uint32_t op = gu_get32(f + 4) >> 16, id = gu_get32(f + 8), len = gu_get32(f + 12);
            uint32_t request_crc = gu_get32(f + 20), out = 0;
            if (!id || (op & GU_REPLY) || gu_get32(f + 16)) continue;
            activity = gz_time_ms();
            if (have_last && id == last_id && op == last_op && request_crc == last_crc) {
                send_frame(op | GU_REPLY, id, last_status, last_reply, last_len); wipe_request(op); continue;
            }
            int status = (!session && op != GU_HELLO) || (have_last && id <= last_id && op != GU_HELLO) ?
                         GZ_EINVAL : dispatch(op, f + GU_HEADER, len, &out);
            last_id = id; last_op = op; last_crc = request_crc; have_last = true;
            last_len = out; last_status = status;
            memcpy(last_reply, payload, out);
            int sent = send_frame(op | GU_REPLY, id, status, payload, out);
            wipe_request(op);
            if (!status && op == GU_USB_NET_OPEN) {
                /* The physical link now has a dedicated Ethernet parser.
                 * Release debugger ownership so TLS can use this IP link. */
                abort_transfer(); parser.used=0; session=have_last=false;
                debugger_state(NULL,NULL);
                break;
            }
            if (!status && !sent && op == GU_BOOT_COMMIT) {
                if (network) gz_sleep_ms(250); /* let lwIP transmit the acknowledged TLS reply */
                int r = gz_usb_ctl(GZ_USB_RELOAD_GO, NULL, 0);
                printf("usb: reload handoff failed (%d)\n", r);
                abort_transfer();
            }
            if (!status && !sent && op == GU_REBOOT_CE) {
                if (network) gz_sleep_ms(250);
                int r = gz_usb_ctl(GZ_USB_REBOOT_CE, NULL, 0);
                printf("usb: CE reboot failed (%d)\n", r);
            }
        }
        transport=network; gz_usb_ctl(GZ_USB_DEBUG_TRANSPORT,&transport,4);
        unsigned wanted = session ? GU_DEBUG_CONNECTED | (network ? GU_DEBUG_NETWORK : 0) : 0;
        if (session && (upload >= 0 || screen || reloading || (int32_t)(gz_time_ms() - copy_until) < 0)) wanted |= GU_DEBUG_COPY;
        if (session && script_state == GU_SCRIPT_RUNNING) wanted |= GU_DEBUG_SCRIPT;
        if (session && mirror_active()) wanted |= GU_DEBUG_MIRROR;
        if (wanted != reported || (session && (int32_t)(gz_time_ms() - report_next) >= 0)) debugger_state(NULL,NULL);
        if (n <= 0) gz_sleep_ms(1);
    }
}
