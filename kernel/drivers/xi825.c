/* Z-Com XI-825 / PRISM station firmware interface.
 * Register/RID/descriptor definitions: Linux hostap HFA384x and the local
 * be300 docs/XI825.md and src/hw/xi825.c. No embedded firmware download.
 *
 * Built a second time as hermes.ko (kernel/drivers/hermes.c defines
 * XI825_HERMES) for Lucent/Agere Hermes cards (WaveLAN/IEEE, Orinoco):
 * the same HFA384x-family host interface (registers, BAP, FIDs, RIDs,
 * TX/RX descriptors). Where Agere firmware differs, the XI825_HERMES
 * branches follow Linux orinoco (hermes_rid.h, hw.c, scan.c): scan results
 * as agere_scan_apinfo, the current TX rate in Mb/s, and WEP through the
 * Agere RIDs.
 * IRQ and syscall paths run with interrupts disabled on this single CPU;
 * command completions/reclaim are polled and are never ACKed by the IRQ path.
 */
#ifndef XI825_TEST
#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"
#include "../mips.h"
#define XR(r) REG16(pcmcia_io_base + 0x300u + (r))   /* hw.h: the socket holding the card */
#define xread(r) XR(r)
#define xwrite(r, v) (XR(r) = (v))
#define xread8(r) REG8(pcmcia_io_base + 0x300u + (r))
#define xwrite8(r, v) (REG8(pcmcia_io_base + 0x300u + (r)) = (v))
#define xcor(v) (REG8(pcmcia_attr_base + 0x3E0u) = (v))
#define xenable() (KJ_SRC_PCMCIA |= KJ_SRC_ENABLE)
#define xdisable() (KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE)
#endif
#include <glaze/wifi.h>
#ifdef XI825_HERMES
#define NAME "hermes"
#define KIND GZ_NET_HERMES
#else
#define NAME "xi825"
#define KIND GZ_NET_XI825
#endif

enum { CMD=0, PARAM=2, STATUS=8, INFOFID=0x10, SELECT=0x18,
       OFFSET=0x1c, RXFID=0x20, ALLOCFID=0x22, EVSTAT=0x30,
       INTEN=0x32, EVACK=0x34, DATA=0x36 };
enum { RX=1, TX=2, TXEXC=4, ALLOC=8, COMMAND=16, INFO=128, INFDROP=0x2000 };
#define QUEUE 8
static bool present;
static int bound_port;
static struct gz_netif_info nic;
static struct wifi_status wifi;
static uint16_t tx_fid;
static bool tx_ready;
static uint64_t association_started, scan_started;
static bool port_on;            /* MAC port 0 enabled (command 1) */
/* The port is on only for an "any" scan (Agere firmware before 8.10), so
 * it is turned off again once the scan is over, before it joins an open
 * network by itself. */
static bool scan_any;
static uint32_t sta_version;    /* station firmware, major << 16 | minor << 8 | variant */
static struct { uint16_t len; uint8_t data[1536]; } queue[QUEUE];
static uint32_t head, tail;
static uint8_t scratch[1600];
static uint16_t get16(const uint8_t *b) { return b[0] | (uint16_t)b[1] << 8; }
static void put16(uint8_t *b, uint16_t v) { b[0] = v; b[1] = v >> 8; }
static void notify(void) { if (bound_port > 0) ipc_notify(bound_port, GZ_NOTIFY_NET); }

/* Deadlines as Linux hostap's (hfa384x): a real PRISM's INIT, its first
 * command after reset, takes hundreds of milliseconds. */
#define WAIT_US         500000u
#define INIT_WAIT_US    2000000u
static int wait_reg_us(unsigned reg, uint16_t mask, uint16_t value, uint32_t us)
{
    uint64_t start = time_us();
    do {
        uint16_t v = xread(reg);
        if (v == 0xffff) return GZ_EIO;     /* the card is gone */
        if ((v & mask) == value) return 0;
    } while (time_us() - start < us);
    return GZ_ETIMEDOUT;
}
static int wait_reg(unsigned reg, uint16_t mask, uint16_t value)
{
    return wait_reg_us(reg, mask, value, WAIT_US);
}
static int command(uint16_t cmd, uint16_t param)
{
    uint32_t us = (cmd & 0x3f) == 0 ? INIT_WAIT_US : WAIT_US;
    if (wait_reg_us(CMD, 0x8000, 0, us)) return GZ_ETIMEDOUT;
    xwrite(EVACK, COMMAND);
    xwrite(PARAM, param);
    xwrite(CMD, cmd);
    if (wait_reg_us(EVSTAT, COMMAND, COMMAND, us)) return GZ_ETIMEDOUT;
    uint16_t status = xread(STATUS);
    xwrite(EVACK, COMMAND);
    return (status & 0x7f00) || (status & 0x3f) != (cmd & 0x3f) ? GZ_EIO : 0;
}
static int bap(uint16_t id, unsigned off, void *data, unsigned len, bool write)
{
    uint8_t *b = data;
    if ((off & 1) || off + len > 4096) return GZ_EINVAL;
    xwrite(SELECT, id);
    xwrite(OFFSET, off);
    if (wait_reg(OFFSET, 0x8000, 0)) return GZ_ETIMEDOUT;
    if (xread(OFFSET) & 0x4000) return GZ_EIO;
    for (unsigned i = 0; i < len; i += 2) {
        if (write) {
            if (i + 1 < len) xwrite(DATA, get16(b + i));
            else {
                xwrite8(DATA, b[i]);
            }
        } else {
            uint16_t v = i + 1 < len ? xread(DATA) : xread8(DATA);
            b[i] = v;
            if (i + 1 < len) b[i + 1] = v >> 8;
        }
    }
    return xread(OFFSET) & 0x4000 ? GZ_EIO : 0;
}
static int read_rid(uint16_t rid, void *out, unsigned len)
{
    uint8_t hdr[4];
    int rv = command(0x21, rid);
    if (rv) return rv;
    if ((rv = bap(rid, 0, hdr, 4, false))) return rv;
    unsigned words = get16(hdr);
    if (get16(hdr + 2) != rid || !words || words > 63 || (words - 1) * 2 < len)
        return GZ_EIO;
    return bap(rid, 4, out, len, false);
}
static int write_rid(uint16_t rid, const void *data, unsigned len)
{
    uint8_t b[128] = {0};
    if (len > 124) return GZ_EINVAL;
    put16(b, 1 + (len + 1) / 2);
    put16(b + 2, rid);
    memcpy(b + 4, data, len);
    int rv = bap(rid, 0, b, 4 + ((len + 1) & ~1u), true);
    return rv ? rv : command(0x121, rid);
}
static int scalar(uint16_t rid, uint16_t value)
{
    uint8_t b[2]; put16(b, value); return write_rid(rid, b, 2);
}

#ifndef XI825_HERMES
static void scan_result(const uint8_t *b, unsigned len)
{
    if (len < 4 || (len - 4) % 62) { wifi.error = GZ_EIO; wifi.scanning = 0; return; }
    wifi.count = 0;
    for (unsigned off = 4; off + 62 <= len && wifi.count < WIFI_AP_MAX; off += 62) {
        const uint8_t *p = b + off;
        unsigned n = get16(p + 16);
        if (n > WIFI_SSID_MAX || !n) continue;
        struct wifi_ap *ap = &wifi.aps[wifi.count++];
        memset(ap, 0, sizeof(*ap));
        ap->ssid_len = n;
        memcpy(ap->ssid, p + 18, n);
        memcpy(ap->bssid, p + 6, 6);
        ap->channel = get16(p);
        ap->noise = get16(p + 2);
        ap->signal = get16(p + 4);
        ap->privacy = (get16(p + 14) & 0x10) != 0;
        ap->rate = get16(p + 60);
    }
    wifi.scanning = 0;
    kprintf(NAME ": scan found %u networks\n", wifi.count);
    notify();
}
/* Host scan results (info 0xF103, station firmware 1.3.1 and later): the
 * record size, a reserved word, then struct hfa384x_hostscan_result
 * records (Linux hostap_common.h): chid, anl, sl, bssid[6], beacon
 * interval, capability, ssid_len, ssid[32], sup_rates[10], rate, atim. */
static void hostscan_result(const uint8_t *b, unsigned len)
{
    unsigned size = len >= 4 ? get16(b) : 0;
    if (size != 64) {
        kprintf(NAME ": host scan record size %u\n", size);
        wifi.error = GZ_EIO; wifi.scanning = 0; notify(); return;
    }
    wifi.count = 0;
    for (unsigned off = 4; off + size <= len && wifi.count < WIFI_AP_MAX; off += size) {
        const uint8_t *p = b + off;
        unsigned n = get16(p + 16);
        if (n > WIFI_SSID_MAX || !n) continue;
        struct wifi_ap *ap = &wifi.aps[wifi.count++];
        memset(ap, 0, sizeof(*ap));
        ap->ssid_len = n;
        memcpy(ap->ssid, p + 18, n);
        memcpy(ap->bssid, p + 6, 6);
        ap->channel = get16(p);
        ap->noise = get16(p + 2);
        ap->signal = get16(p + 4);
        ap->privacy = (get16(p + 14) & 0x10) != 0;
        ap->rate = get16(p + 60);
    }
    wifi.scanning = 0;
    kprintf(NAME ": host scan found %u networks\n", wifi.count);
    notify();
}
#endif
#ifdef XI825_HERMES
/* Agere scan results (info 0xF101): no header, then 50-byte records:
 * channel, noise, level, BSSID, beacon interval, capabilities, SSID
 * (length + 32 bytes), and no rates. Read from a real Orinoco Gold (8.72.1,
 * 200 bytes for 4 networks) and WaveLAN Gold (6.4.1, 350 bytes for 7);
 * Linux orinoco's agere_scan_apinfo. The firmware repeats a network it
 * heard more than once, so records with a BSSID already listed are skipped. */
static void agere_scan_result(const uint8_t *b, unsigned len)
{
    wifi.count = 0;
    for (unsigned off = 0; off + 50 <= len && wifi.count < WIFI_AP_MAX; off += 50) {
        const uint8_t *p = b + off;
        unsigned channel = get16(p), n = get16(p + 16);
        if (channel < 1 || channel > 14 || n < 1 || n > WIFI_SSID_MAX)
            continue;
        bool seen = false;
        for (unsigned i = 0; i < wifi.count && !seen; i++)
            seen = !memcmp(wifi.aps[i].bssid, p + 6, 6);
        if (seen)
            continue;
        struct wifi_ap *ap = &wifi.aps[wifi.count++];
        memset(ap, 0, sizeof(*ap));
        ap->ssid_len = n;
        memcpy(ap->ssid, p + 18, n);
        memcpy(ap->bssid, p + 6, 6);
        ap->channel = channel;
        ap->noise = get16(p + 2);
        ap->signal = get16(p + 4);
        ap->privacy = (get16(p + 14) & 0x10) != 0;
        ap->rate = 22;                          /* 802.11b: 11 Mb/s, in 500 kb/s */
    }
    wifi.scanning = 0;
    kprintf(NAME ": scan found %u networks\n", wifi.count);
    notify();
}
#endif
static void information(void)
{
    uint16_t fid = xread(INFOFID);
    uint8_t hdr[4];
    if (bap(fid, 0, hdr, 4, false)) return;
    unsigned words = get16(hdr);
    if (!words || words > 799) { wifi.error = GZ_EIO; return; }
    unsigned len = (words - 1) * 2;
    /* An empty frame (an old Agere scan with nothing to report) has no
     * body to read. */
    if (len && bap(fid, 4, scratch, len, false)) return;
    if (get16(hdr + 2) != 0xf200)
        kprintf(NAME ": info %x, %u bytes\n", get16(hdr + 2), len);
#ifdef XI825_HERMES
    if (get16(hdr + 2) == 0xf101) agere_scan_result(scratch, len);
#else
    if (get16(hdr + 2) == 0xf101) scan_result(scratch, len);
#endif
#ifndef XI825_HERMES
    else if (get16(hdr + 2) == 0xf103) hostscan_result(scratch, len);
#endif
    else if (get16(hdr + 2) == 0xf200 && len >= 2) {
        unsigned state = get16(scratch);
        if (wifi.state == WIFI_DISCONNECTED || wifi.state == WIFI_FAILED) return;
        if (state == 1 || state == 5) {
            wifi.state = WIFI_CONNECTED;
            wifi.error = 0;
        } else if (wifi.state == WIFI_CONNECTED) {
            wifi.state = WIFI_FAILED;
            wifi.error = GZ_ENETDOWN;
            memset(&wifi.current, 0, sizeof(wifi.current));
            head = tail = 0;
        }
        notify();
    }
}
static void receive(void)
{
    uint16_t fid = xread(RXFID);
    uint8_t hdr[60];
    if (bap(fid, 0, hdr, 60, false)) { nic.rx_dropped++; return; }
    unsigned len = get16(hdr + 44);
    if (wifi.state != WIFI_CONNECTED || (get16(hdr) & 7) || len < 8 || len > 1508 || head - tail >= QUEUE) {
        nic.rx_dropped++; return;
    }
    if (bap(fid, 60, scratch, len, false)) { nic.rx_dropped++; return; }
    unsigned n;
    uint8_t *out = queue[head % QUEUE].data;
    memcpy(out, hdr + 46, 12);
    if (scratch[0] == 0xaa && scratch[1] == 0xaa && scratch[2] == 3 &&
        !scratch[3] && !scratch[4] && (scratch[5] == 0 || scratch[5] == 0xf8)) {
        n = len + 6;
        memcpy(out + 12, scratch + 6, len - 6);
    } else {
        n = len + 14;
        if (n > GZ_ETH_FRAME_MAX) { nic.rx_dropped++; return; }
        memcpy(out + 12, hdr + 58, 2);
        memcpy(out + 14, scratch, len);
    }
    queue[head++ % QUEUE].len = n;
    nic.rx_frames++;
    notify();
}
void xi825_irq(void)
{
    if (!present) return;
    for (unsigned i = 0; i < 16; i++) {
        uint16_t ev = xread(EVSTAT);
        if (ev == 0xffff) break;            /* the card is gone */
        uint16_t events = ev & (RX | INFO | INFDROP);
        if (!events) break;
        if (events & RX) receive();
        if (events & INFO) information();
        if (events & INFDROP) nic.overruns++;
        xwrite(EVACK, events);
    }
    if (wifi.scanning && time_us() - scan_started > 5000000) {
        wifi.scanning = 0; wifi.error = GZ_ETIMEDOUT; notify();
    }
    if (scan_any && !wifi.scanning) {
        command(2, 0);
        port_on = scan_any = false;
    }
    if (wifi.state == WIFI_CONNECTING && time_us() - association_started > 10000000) {
        command(2, 0);
        port_on = false;
        wifi.state = WIFI_FAILED; wifi.error = GZ_ETIMEDOUT;
        memset(&wifi.current, 0, sizeof(wifi.current));
        notify();
    }
}
void xi825_init(void)
{
    xcor(1); /* PRISM COR at attribute byte 0x3e0; avoids NAND's 0xD200 port. */
    xwrite(INTEN, 0);
    int rv = command(0, 0);
    if (!rv) rv = read_rid(0xfc01, nic.mac, 6);
    if (!rv) {
        /* STAID: component id, variant, major, minor (hostap_common.h). */
        uint8_t id[8];
        if (!read_rid(0xfd20, id, sizeof(id))) {
            sta_version = (uint32_t)get16(id + 4) << 16 | get16(id + 6) << 8 | get16(id + 2);
            kprintf(NAME ": station firmware %u.%u.%u (id %x)\n", get16(id + 4), get16(id + 6),
                    get16(id + 2), get16(id));
        }
    }
    if (!rv) rv = scalar(0xfc00, 1); /* infrastructure */
#ifdef XI825_HERMES
    if (!rv) rv = scalar(0xfc20, 0); /* WEP off (CNFWEPENABLED_AGERE) */
#else
    if (!rv) rv = scalar(0xfc28, 0); /* open */
#endif
    if (!rv) rv = command(0x0a, sizeof(scratch));
    if (!rv) rv = wait_reg(EVSTAT, ALLOC, ALLOC);
    if (rv) {
        kprintf(NAME ": initialization failed %d (CMD %x EVSTAT %x STATUS %x)\n", rv,
                xread(CMD), xread(EVSTAT), xread(STATUS));
        return;
    }
    tx_fid = xread(ALLOCFID);
    tx_ready = true;
    xwrite(EVACK, ALLOC);
    present = true;
    nic.present = 1; nic.kind = KIND;
    wifi.state = WIFI_DISCONNECTED;
    wifi.capabilities = WIFI_CAP(WIFI_OPEN) | WIFI_CAP(WIFI_WEP);
    xwrite(INTEN, RX | INFO | INFDROP);
    xenable();
    kprintf(NAME ": MAC %02x:%02x:%02x:%02x:%02x:%02x (disconnected)\n",
            nic.mac[0], nic.mac[1], nic.mac[2], nic.mac[3], nic.mac[4], nic.mac[5]);
}
bool xi825_present(void) { return present; }

/* The card was pulled: forget it, its link and its queued frames, and mask
 * its interrupt, so that a card inserted later starts from scratch. */
void xi825_removed(void)
{
    xdisable();
    present = false;
    tx_ready = false;
    port_on = scan_any = false;
    head = tail = 0;
    memset(&nic, 0, sizeof(nic));
    memset(&wifi, 0, sizeof(wifi));
    notify();
}
static int check(struct proc *p)
{
    return !(p->flags & PROC_NET) ? GZ_EPERM : present ? 0 : GZ_ENOENT;
}
int xi825_info(struct proc *p, uint32_t out)
{
    if (!(p->flags & PROC_NET)) return GZ_EPERM;
    return copy_to_as(&p->as, out, &nic, sizeof(nic));
}
int xi825_bind(struct proc *p, int port)
{
    int rv = check(p); if (rv) return rv;
    bound_port = port;
    if (head != tail) notify();
    return 0;
}
static int send_frame(struct proc *p, uint32_t buf, uint32_t len)
{
    int rv = check(p); if (rv) return rv;
    if (len < 14 || len > GZ_ETH_FRAME_MAX) return GZ_EINVAL;
    if (wifi.state != WIFI_CONNECTED) return GZ_ENETDOWN;
    if (!tx_ready) {
        if (!(xread(EVSTAT) & ALLOC)) return GZ_EBUSY;
        if (xread(ALLOCFID) != tx_fid) return GZ_EIO;
        xwrite(EVACK, ALLOC); tx_ready = true;
    }
    /* 60-byte firmware descriptor, then SNAP + Ethernet payload. */
    memset(scratch, 0, 68);
    if (copy_from_as(&p->as, scratch + 66, buf + 12, len - 12)) return GZ_EINVAL;
    if (copy_from_as(&p->as, scratch + 46, buf, 12)) return GZ_EINVAL;
    unsigned payload = len - 14 + 8;
    put16(scratch + 12, 6); /* request TX/TXEXC, plus reclaim in command */
    put16(scratch + 44, payload);
    scratch[58] = payload >> 8; scratch[59] = payload;
    scratch[60] = 0xaa; scratch[61] = 0xaa; scratch[62] = 3;
    if ((rv = bap(tx_fid, 0, scratch, 60 + payload, true))) return rv;
    xwrite(EVACK, TX | TXEXC);
    tx_ready = false;
    if ((rv = command(0x10b, tx_fid))) return rv;
    if ((rv = wait_reg(EVSTAT, ALLOC, ALLOC))) return rv;
    if (xread(ALLOCFID) != tx_fid) return GZ_EIO;
    tx_ready = true;
    uint16_t ev = xread(EVSTAT);
    xwrite(EVACK, ALLOC | TX | TXEXC);
    if (ev & TXEXC) return GZ_EIO;
    nic.tx_frames++;
    return len;
}
int xi825_send(struct proc *p, uint32_t buf, uint32_t len)
{
    uint32_t flags = irq_save();
    int rv = send_frame(p, buf, len);
    irq_restore(flags); return rv;
}
int xi825_recv(struct proc *p, uint32_t buf, uint32_t max)
{
    int rv = check(p); if (rv) return rv;
    uint32_t flags = irq_save();
    if (head == tail) rv = 0;
    else {
        unsigned n = MIN(queue[tail % QUEUE].len, max);
        rv = copy_to_as(&p->as, buf, queue[tail % QUEUE].data, n);
        if (!rv) { tail++; rv = n; }
    }
    irq_restore(flags); return rv;
}
static int control(struct proc *p, uint32_t op, uint32_t buf)
{
    int rv = check(p); if (rv) return rv;
    xi825_irq();
    if (op == WIFI_STATUS) {
        if (wifi.state == WIFI_CONNECTED) {
            uint8_t b[34];
            if ((rv = read_rid(0xfd41, b, 34))) return rv;
            unsigned n = get16(b);
            if (n > WIFI_SSID_MAX) return GZ_EIO;
            wifi.current.ssid_len = n;
            memcpy(wifi.current.ssid, b + 2, n);
            if ((rv = read_rid(0xfd42, wifi.current.bssid, 6))) return rv;
            if ((rv = read_rid(0xfd43, b, 6))) return rv;
            wifi.current.signal = get16(b + 2); wifi.current.noise = get16(b + 4);
            if ((rv = read_rid(0xfd44, b, 2))) return rv;
            unsigned rate = get16(b);
#ifdef XI825_HERMES
            wifi.current.rate = rate == 11 ? 22 : rate == 6 ? 11 : rate == 2 ? 4 : rate == 1 ? 2 : 0;
#else
            wifi.current.rate = rate == 8 ? 22 : rate == 4 ? 11 : rate == 2 ? 4 : rate == 1 ? 2 : 0;
#endif
            if ((rv = read_rid(0xfdc1, b, 2))) return rv;
            wifi.current.channel = get16(b);
        }
        return copy_to_as(&p->as, buf, &wifi, sizeof(wifi));
    }
    if (op == WIFI_SCAN) {
        if (wifi.scanning) return GZ_EBUSY;
        /* Station firmware answers the scan inquiry only with its port
         * enabled (Linux hostap, prism2_request_scan): enable it for an SSID
         * no network has, so that it scans without joining anything. */
        if (!port_on) {
            uint8_t ssid[34];
            put16(ssid, WIFI_SSID_MAX);
            for (unsigned i = 0; i < WIFI_SSID_MAX; i++)
                ssid[2 + i] = (uint8_t)(0xA5 ^ i);
#ifdef XI825_HERMES
            /* Agere firmware before 8.10 has no host scan (Linux orinoco
             * hw.c, has_hostscan): its scan inquiry lists only what it saw
             * looking for the desired SSID, so it looks for any (length 0),
             * as Linux leaves it. A real WaveLAN Gold (6.4.1) returned no
             * networks for the made-up SSID. It may then join an open
             * network by itself. */
            bool any = sta_version < 0x080a00;
            if (any)
                memset(ssid, 0, sizeof(ssid));
#else
            bool any = false;
#endif
            if (!(rv = write_rid(0xfc02, ssid, sizeof(ssid))) && !(rv = command(1, 0))) {
                port_on = true;
                scan_any = any;
            }
            if (rv) kprintf(NAME ": enabling the port to scan failed %d\n", rv);
        }
        wifi.error = 0; wifi.scanning = 1; scan_started = time_us();
        /* Firmware 1.3.1 and later scans all channels on a host scan
         * request (hostap prism2_request_hostscan: channels 1-14, probes
         * at 1 Mbps, any SSID); the scan inquiry only reports networks
         * matching the desired SSID. Older firmware, or one refusing the
         * RID, gets the inquiry. */
        rv = GZ_ENOENT;
#ifdef XI825_HERMES
        if (sta_version >= 0x080a00) {      /* host scan: firmware 8.10 on */
            uint8_t any[34] = {0};
            rv = write_rid(0xfcb2, any, sizeof(any));
            if (rv) kprintf(NAME ": scan SSID refused %d\n", rv);
            rv = GZ_ENOENT;
        }
#else
        if (sta_version >= 0x010301) {
            uint8_t req[38] = {0};
            put16(req, 0x3fff);
            put16(req + 2, 1);
            rv = write_rid(0xfce5, req, sizeof(req));
            kprintf(NAME ": host scan requested %d\n", rv);
        }
#endif
        if (rv) {
            rv = command(0x11, 0xf101);
            kprintf(NAME ": scan requested %d\n", rv);
        }
        if (rv) { wifi.scanning = 0; wifi.error = rv; }
        return rv;
    }
    if (op != WIFI_CONNECT && op != WIFI_CONFIGURE && op != WIFI_DISCONNECT) return GZ_EINVAL;
    struct wifi_config config = {0};
    if (op == WIFI_CONFIGURE) {
        if (copy_from_as(&p->as, &config, buf, sizeof(config))) return GZ_EINVAL;
        if (config.security > WIFI_WEP) return WIFI_EUNSUPPORTED;
        if (!config.network.ssid_len || config.network.ssid_len > WIFI_SSID_MAX ||
            config.key_index > 3 || config.shared_auth > 1 ||
            (config.security == WIFI_WEP && config.key_len != 5 && config.key_len != 13)) return GZ_EINVAL;
    }
    struct wifi_request req;
    if (op == WIFI_CONFIGURE) req = config.network;
    if (op == WIFI_CONNECT) {
        if (copy_from_as(&p->as, &req, buf, sizeof(req))) return GZ_EINVAL;
        if (!req.ssid_len || req.ssid_len > WIFI_SSID_MAX) return GZ_EINVAL;
    }
    if ((rv = command(2, 0))) return rv;
    port_on = scan_any = false;
    wifi.state = WIFI_DISCONNECTED; wifi.error = 0;
    head = tail = 0;
    memset(&wifi.current, 0, sizeof(wifi.current));
    /* Drain stale association/RX events before beginning another attempt. */
    xi825_irq();
    /* Erase every firmware key when leaving or replacing a network. */
#ifdef XI825_HERMES
    /* Agere: four { length, 14 bytes } keys in CNFWEPKEYS_AGERE (0xFC25),
     * WEP enable 0xFC20, authentication 0xFC21 (orinoco hermes_rid.h). */
    uint8_t keys[64] = {0};
    if ((rv = write_rid(0xfc25, keys, sizeof(keys))) || (rv = scalar(0xfc20, 0)) ||
        (rv = scalar(0xfc21, 1))) return rv;
#else
    uint8_t zero[14] = {0};
    for (unsigned i = 0; i < 4; i++) {
        rv = write_rid(0xfc24 + i, zero, sizeof(zero));
        if (rv) return rv;
    }
    if ((rv = scalar(0xfc28, 0)) || (rv = scalar(0xfc2a, 1))) return rv;
#endif
    wifi.security = WIFI_OPEN;
    if (op == WIFI_DISCONNECT) { notify(); return 0; }
#ifdef XI825_HERMES
    if (config.security == WIFI_WEP) {
        /* __orinoco_hw_setup_wepkeys and __orinoco_hw_setup_enc, Agere:
         * the key table, the TX key (CNFTXKEY_AGERE 0xFCB1), shared or
         * open authentication, then WEP enabled. */
        put16(keys + 16 * config.key_index, config.key_len);
        memcpy(keys + 16 * config.key_index + 2, config.key, config.key_len);
        rv = write_rid(0xfc25, keys, sizeof(keys));
        memset(keys, 0, sizeof(keys)); memset(config.key, 0, sizeof(config.key));
        if (!rv) rv = scalar(0xfcb1, config.key_index);
        if (!rv) rv = scalar(0xfc21, config.shared_auth ? 2 : 1);
        if (!rv) rv = scalar(0xfc20, 1);
        if (rv) return rv;
        wifi.security = WIFI_WEP;
    }
#else
    if (config.security == WIFI_WEP) {
        /* PRISM requires all four default-key RIDs to have the same size,
         * including unused slots (Linux hostap_set_encryption). */
        uint8_t key[14];
        unsigned length = config.key_len == 5 ? 6 : 14;
        for (unsigned i = 0; i < 4 && !rv; i++) {
            memset(key, 0, sizeof(key));
            if (i == config.key_index) memcpy(key, config.key, config.key_len);
            rv = write_rid(0xfc24 + i, key, length);
        }
        memset(key, 0, sizeof(key)); memset(config.key, 0, sizeof(config.key));
        if (!rv) rv = scalar(0xfc23, config.key_index);
        if (!rv) rv = scalar(0xfc2a, config.shared_auth ? 2 : 1);
        if (!rv) rv = scalar(0xfc28, 3); /* privacy invoked, exclude unencrypted */
        if (rv) return rv;
        wifi.security = WIFI_WEP;
    }
#endif
    uint8_t ssid[34] = {0};
    put16(ssid, req.ssid_len); memcpy(ssid + 2, req.ssid, req.ssid_len);
    if (!(rv = write_rid(0xfc02, ssid, sizeof(ssid)))) rv = command(1, 0);
    port_on = !rv;
    wifi.state = rv ? WIFI_FAILED : WIFI_CONNECTING;
    wifi.error = rv; association_started = time_us();
    notify(); return rv;
}
int xi825_control(struct proc *p, uint32_t op, uint32_t buf)
{
    uint32_t flags = irq_save();
    int rv = control(p, op, buf);
    irq_restore(flags); return rv;
}
