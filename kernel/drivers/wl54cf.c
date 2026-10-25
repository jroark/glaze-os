/* Marvell CF8385 host driver. Protocol references: docs/WL54CF.md.
 * All integers on the wire are explicitly little endian; no unaligned loads.
 * Firmware commands run synchronously during boot. Runtime IRQs move frames.
 */
#include "../drivers.h"
#include "../hw.h"
#include "../kernel.h"
#include <glaze/wifi.h>

#include "wl54cf_io.h"
#define TX 1
#define RX 2
#define COMMAND 4
#define RESPONSE 8
#define EVENT 16
#define BUFFER 2048
#define TIMEOUT_US 500000u
static uint8_t cmd[BUFFER], buf[BUFFER];
static uint16_t sequence;
static bool online;
static struct wifi_status wifi;
static bool authorized, pairwise_key, group_key;
static uint16_t ap_capability[WIFI_AP_MAX];
static uint8_t ap_rates[WIFI_AP_MAX][14], ap_rates_len[WIFI_AP_MAX];
static uint16_t get16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }
static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}
static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}
static bool wait_bits(unsigned reg, uint16_t bits)
{
    uint64_t start = time_us();
    do {
        uint16_t v = wl_read16(reg);
        if (v != 0xffff && (v & bits) == bits)
            return true;
    } while (time_us() - start < TIMEOUT_US);
    return false;
}
static void write_fifo(unsigned reg, const uint8_t *p, unsigned n)
{
    unsigned i = 0;
    for (; i + 1 < n; i += 2)
        wl_write16(reg, get16(p + i));
    if (i < n)
        wl_write8(reg, p[i]);
}
static void read_fifo(unsigned reg, uint8_t *p, unsigned n)
{
    unsigned i = 0;
    for (; i + 1 < n; i += 2) {
        uint16_t w = wl_read16(reg);
        put16(p + i, w);
    }
    if (i < n)
        p[i] = wl_read8(reg);
}
static void signal(uint16_t bits)
{
    wl_write16(0, bits);
    wl_write16(2, bits);
}
static bool firmware(void)
{
    const struct romfs_entry *helper = romfs_lookup("firmware/cf8385_helper.bin");
    const struct romfs_entry *fw = romfs_lookup("firmware/cf8385.bin");
    if (!helper || !fw) {
        kputs("wl54cf: missing firmware; extract vendor CAB and build WL54CF=1\n");
        return false;
    }
    if (wl_read8(0x3f) == 0x5a)
        return true;
    if (wl_read8(0x3f) != 0 || !helper->size || helper->size > 65536 || !fw->size ||
        fw->size > 512 * 1024)
        return false;
    for (uint32_t pos = 0; pos < helper->size;) {
        unsigned n = MIN(256u, helper->size - pos);
        if ((n & 1) || romfs_read(helper, pos, buf, n))
            return false;
        wl_write16(0x18, n);
        write_fifo(0x1a, buf, n);
        signal(COMMAND);
        if (!wait_bits(0x20, COMMAND))
            return false;
        pos += n;
    }
    wl_write16(0x18, 0);
    signal(COMMAND);
    if (!wait_bits(0x20, COMMAND))
        return false;
    uint64_t start = time_us();
    while (wl_read16(0x28) != 16)
        if (time_us() - start > TIMEOUT_US)
            return false;
    uint32_t pos = 0, previous = 0;
    unsigned retries = 0;
    while (pos < fw->size) {
        unsigned n = wl_read16(0x28);
        if (n & 1) {
            if (!previous || previous > pos || (n & ~1u) != previous || ++retries > 20)
                return false;
            pos -= previous;
            n &= ~1u;
        } else
            retries = 0;
        if (!n || n > sizeof(buf) || n > fw->size - pos || romfs_read(fw, pos, buf, n))
            return false;
        wl_write16(0x18, n);
        write_fifo(0x1a, buf, n);
        signal(COMMAND);
        if (!wait_bits(0x20, COMMAND))
            return false;
        pos += n;
        previous = n;
    }
    start = time_us();
    while (wl_read8(0x3f) != 0x5a)
        if (time_us() - start > TIMEOUT_US)
            return false;
    kputs("wl54cf: firmware ready\n");
    return true;
}
static unsigned command(uint16_t op, unsigned n)
{
    if (n < 8 || n > sizeof(cmd))
        return 0;
    if (!wait_bits(0x20, COMMAND)) {
        kprintf("wl54cf: command %x: card not ready (status %x)\n", op, wl_read16(0x20));
        return 0;
    }
    put16(cmd, op);
    put16(cmd + 2, n);
    put16(cmd + 4, ++sequence);
    put16(cmd + 6, 0);
    wl_write16(0x18, n);
    write_fifo(0x1a, cmd, n);
    signal(COMMAND);
    /* A real card's scan sweeps every channel, and authentication and
     * association wait for the access point: seconds, not milliseconds. */
    uint32_t deadline = op == 6 || op == 0x11 || op == 0x12 || op == 0x50 ? 3000000u : TIMEOUT_US;
    uint64_t t0 = time_us();
    while (!(wl_read16(0x22) & RESPONSE))
        if (time_us() - t0 > deadline) {
            kprintf("wl54cf: command %x: no response (cause %x)\n", op, wl_read16(0x22));
            return 0;
        }
    /* As Linux libertas if_cs_receive_cmdres: the response is in the card
     * once CARD_STATUS (0x20) shows it and RESP_LEN is set. A real WL54-CF
     * raises the cause bit first and reads a length of 0 until then. */
    unsigned count = 0;
    for (t0 = time_us(); time_us() - t0 < deadline;)
        if ((wl_read16(0x20) & RESPONSE) && (count = wl_read16(0x30)) != 0)
            break;
    if (count < 16 || count > sizeof(buf)) {
        kprintf("wl54cf: command %x: response length %u\n", op, count);
        wl_write16(0x22, RESPONSE);
        return 0;
    }
    read_fifo(0x12, buf, count);
    wl_write16(0x22, RESPONSE);
    unsigned length = get16(buf + 2);
    /* A real WL54-CF (firmware 2.5) answers ASSOCIATE 0x0050 with the older
     * command's code, 0x8012. */
    uint16_t code = get16(buf);
    bool answers = code == (op | 0x8000) || (op == 0x50 && code == 0x8012);
    if (!answers || get16(buf + 4) != sequence || get16(buf + 6) || length < 8 ||
        length > count - 8) {
        kprintf("wl54cf: command %x (seq %u): response %u bytes: %04x %04x %04x %04x\n", op,
                sequence, count, get16(buf), length, get16(buf + 4), get16(buf + 6));
        return 0;
    }
    return length;
}
/* Parse standard SCAN response, including all bounds before dereferencing IEs. */
/* One SCAN command (CMD_802_11_SCAN) over `count` channels from `first`,
 * as Linux libertas lbs_scan sends it: any BSS type and BSSID, then a
 * wildcard SSID TLV, a channel list TLV (active scan, 100 ms per channel)
 * and the b/g rates TLV. A real WL54-CF answers no SCAN without them. The
 * results are added to wifi.aps, one entry per BSSID. */
static bool scan_channels(unsigned first, unsigned count)
{
    static const uint8_t rates[] = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c};
    memset(cmd, 0, sizeof(cmd)); cmd[8] = 3;
    unsigned p = 15;
    put16(cmd + p, 0); put16(cmd + p + 2, 0); p += 4;               /* SSID: any */
    put16(cmd + p, 0x0101); put16(cmd + p + 2, count * 7); p += 4;  /* channel list */
    for (unsigned c = first; c < first + count; c++) {
        cmd[p] = 0; cmd[p + 1] = (uint8_t)c; cmd[p + 2] = 0;        /* b/g, active */
        put16(cmd + p + 3, 0); put16(cmd + p + 5, 100); p += 7;
    }
    put16(cmd + p, 0x0001); put16(cmd + p + 2, sizeof(rates)); p += 4;
    memcpy(cmd + p, rates, sizeof(rates)); p += sizeof(rates);
    unsigned n = command(6, p);
    /* bssdescriptsize, nr_sets, the descriptors, then optional TLVs. */
    if (n < 11 || get16(buf + 8) > n - 11) return false;
    unsigned end = 11 + get16(buf + 8), pos = 11;
    for (unsigned b = 0; b < buf[10]; b++) {
        if (pos + 2 > end) return false;
        unsigned length = get16(buf + pos); pos += 2;
        if (length < 19 || length > end - pos) return false;
        unsigned stop = pos + length;
        struct wifi_ap entry = {0}; uint8_t ap_rate[14]; unsigned nr = 0;
        uint16_t capability = get16(buf + pos + 17);
        memcpy(entry.bssid, buf + pos, 6);
        entry.signal = buf[pos + 6]; entry.privacy = !!(capability & 16);
        unsigned ie = pos + 19;
        while (ie < stop) {
            if (stop - ie < 2) return false;
            unsigned tag = buf[ie], l = buf[ie + 1]; ie += 2;
            if (l > stop - ie) return false;
            if (!tag && l && l <= WIFI_SSID_MAX) {
                entry.ssid_len = l; memcpy(entry.ssid, buf + ie, l);
            } else if (tag == 3 && l == 1) entry.channel = buf[ie];
            else if (tag == 1 || tag == 50) {
                for (unsigned r = 0; r < l && nr < sizeof(ap_rate); r++) ap_rate[nr++] = buf[ie+r];
            } else if ((tag == 48 || (tag == 221 && l >= 4 &&
                        !memcmp(buf + ie, "\x00\x50\xf2\x01", 4))) && l + 2 <= WIFI_IE_MAX) {
                if (!entry.ie_len || tag == 48) {
                    entry.ie_len = l + 2; memcpy(entry.ie, buf + ie - 2, l + 2);
                }
            }
            ie += l;
        }
        unsigned idx = 0;
        while (idx < wifi.count && memcmp(wifi.aps[idx].bssid, entry.bssid, 6))
            idx++;
        if (entry.ssid_len && (capability & 1) && idx < WIFI_AP_MAX) {
            wifi.aps[idx] = entry; ap_capability[idx] = capability;
            memcpy(ap_rates[idx], ap_rate, nr); ap_rates_len[idx] = nr;
            if (idx == wifi.count)
                wifi.count++;
        }
        pos = stop;
    }
    return true;
}
static bool scan(void)
{
    bool any = false;
    wifi.count = 0;
    for (unsigned c = 1; c <= 13; c += 4)
        any |= scan_channels(c, MIN(4u, 14 - c));
    return any;
}
static int key_material(const struct wifi_key *key)
{
    if (key->cipher > WIFI_KEY_CCMP || key->index > 3 || key->pairwise > 1 ||
        key->sequence_len > 8 ||
        (key->cipher == WIFI_KEY_TKIP && key->length != 32) ||
        (key->cipher == WIFI_KEY_CCMP && key->length != 16) ||
        (key->cipher == WIFI_KEY_NONE && key->length)) return GZ_EINVAL;
    memset(cmd, 0, 52);
    put16(cmd + 8, 1); put16(cmd + 10, 0x100); put16(cmd + 12, 38);
    put16(cmd + 14, key->cipher);
    put16(cmd + 16, (key->pairwise ? 2 : 1) | (key->length ? 4 : 0));
    put16(cmd + 18, key->length); memcpy(cmd + 20, key->key, key->length);
    bool ok = command(0x5e, 52) != 0;
    memset(cmd, 0, sizeof(cmd)); memset(buf, 0, sizeof(buf));
    return ok ? 0 : GZ_EIO;
}
static int clear_keys(void)
{
    struct wifi_key key = {0};
    int rv = key_material(&key); key.pairwise = 1;
    if (!rv) rv = key_material(&key);
    memset(cmd, 0, 80); put16(cmd + 8, 4); /* remove WEP */
    if (!command(0x13, 80)) rv = GZ_EIO;
    authorized = pairwise_key = group_key = false;
    return rv;
}
static int disconnect(void)
{
    authorized = pairwise_key = group_key = false;
    memset(cmd, 0, 16); memcpy(cmd + 8, wifi.current.bssid, 6); put16(cmd + 14, 3);
    bool ok = command(0x24, 16) != 0;
    memset(&wifi.current, 0, sizeof(wifi.current)); wifi.state = WIFI_DISCONNECTED;
    int rv = clear_keys();
    return ok ? rv : GZ_EIO;
}
static int associate(const struct wifi_config *c)
{
    if (!c->network.ssid_len || c->network.ssid_len > WIFI_SSID_MAX || c->security > WIFI_WPA2_PSK ||
        c->key_index > 3 || c->shared_auth > 1 || c->ie_len > WIFI_IE_MAX ||
        (c->security == WIFI_WEP && c->key_len != 5 && c->key_len != 13)) return GZ_EINVAL;
    if (!(wifi.capabilities & WIFI_CAP(c->security))) return WIFI_EUNSUPPORTED;
    int rv = disconnect(); if (rv) return rv;
    if (!scan()) return GZ_EIO;
    unsigned i;
    for (i = 0; i < wifi.count; i++)
        if (wifi.aps[i].ssid_len == c->network.ssid_len &&
            !memcmp(wifi.aps[i].ssid, c->network.ssid, c->network.ssid_len) &&
            (!memcmp(c->bssid, "\0\0\0\0\0\0", 6) || !memcmp(c->bssid, wifi.aps[i].bssid, 6))) break;
    if (i == wifi.count) return GZ_ENOENT;
    const struct wifi_ap *ap = &wifi.aps[i];
    if (!!ap->privacy != (c->security != WIFI_OPEN)) return WIFI_EUNSUPPORTED;
    if (c->security == WIFI_WEP) {
        memset(cmd, 0, 80); put16(cmd+8, 2); put16(cmd+10, c->key_index);
        cmd[12+c->key_index] = c->key_len == 5 ? 1 : 2;
        memcpy(cmd+16+16*c->key_index, c->key, c->key_len);
        if (!command(0x13, 80)) return GZ_EIO;
        memset(cmd, 0, sizeof(cmd)); memset(buf, 0, sizeof(buf));
    }
    memset(cmd, 0, 12); put16(cmd+8, 1); put16(cmd+10, c->security >= WIFI_WPA_PSK);
    if (!command(0x2f, 12)) return WIFI_EUNSUPPORTED;
    memset(cmd, 0, 12); put16(cmd+8, 0x43 | (c->security == WIFI_WEP ? 8 : 0));
    if (!command(0x28, 12)) return GZ_EIO;
    memset(cmd, 0, 25); memcpy(cmd+8, ap->bssid, 6); cmd[14] = c->shared_auth;
    if (!command(0x11, 25)) {
        kputs("wl54cf: authentication got no answer\n");
        return WIFI_EAUTH;
    }
    memset(cmd, 0, sizeof(cmd)); memcpy(cmd+8, ap->bssid, 6);
    put16(cmd+14, ap_capability[i] & 0x11); put16(cmd+16, 10); put16(cmd+18, 100);
    unsigned p = 21;
    put16(cmd+p, 0); put16(cmd+p+2, ap->ssid_len); memcpy(cmd+p+4, ap->ssid, ap->ssid_len); p += 4+ap->ssid_len;
    put16(cmd+p, 3); put16(cmd+p+2, 1); cmd[p+4] = ap->channel; p += 5;
    put16(cmd+p, 4); put16(cmd+p+2, 6); p += 10;
    put16(cmd+p, 1); put16(cmd+p+2, ap_rates_len[i]); memcpy(cmd+p+4, ap_rates[i], ap_rates_len[i]); p += 4+ap_rates_len[i];
    if (c->security >= WIFI_WPA_PSK) {
        if (c->ie_len < 2 || (unsigned)c->ie[1] + 2 != c->ie_len) return GZ_EINVAL;
        put16(cmd+p, c->ie[0]); put16(cmd+p+2, c->ie_len-2);
        memcpy(cmd+p+4, c->ie+2, c->ie_len-2); p += c->ie_len+2;
    }
    unsigned answer = command(0x50, p);
    if (answer < 14 || get16(buf+10)) {
        if (answer)
            kprintf("wl54cf: association refused, status %x\n", get16(buf + 10));
        return WIFI_EAUTH;
    }
    wifi.current = *ap; wifi.current.rate = 22; wifi.security = c->security;
    authorized = c->security < WIFI_WPA_PSK;
    wifi.state = authorized ? WIFI_CONNECTED : WIFI_AUTHENTICATING;
    return 0;
}
int wl54cf_control(uint32_t op, void *data)
{
    if (!online) return GZ_ENOENT;
    if (op == WIFI_STATUS) { memcpy(data, &wifi, sizeof(wifi)); return 0; }
    if (op == WIFI_SCAN) { wifi.error = scan() ? 0 : GZ_EIO; return wifi.error; }
    if (op == WIFI_DISCONNECT) return disconnect();
    if (op == WIFI_SET_KEY) {
        if (wifi.state != WIFI_AUTHENTICATING && wifi.state != WIFI_CONNECTED) return GZ_ENETDOWN;
        const struct wifi_key *key = data;
        if ((wifi.security == WIFI_WPA_PSK && key->cipher != WIFI_KEY_TKIP) ||
            (wifi.security == WIFI_WPA2_PSK && key->cipher != WIFI_KEY_CCMP && key->cipher != WIFI_KEY_TKIP)) return WIFI_EUNSUPPORTED;
        int rv = key_material(key);
        if (!rv) { if (key->pairwise) pairwise_key = true; else group_key = true; }
        return rv;
    }
    if (op == WIFI_AUTHORIZE) {
        if (!pairwise_key || !group_key || (wifi.state != WIFI_AUTHENTICATING && wifi.state != WIFI_CONNECTED)) return GZ_EPERM;
        authorized = true; wifi.state = WIFI_CONNECTED; return 0;
    }
    if (op == WIFI_CONNECT || op == WIFI_CONFIGURE) {
        struct wifi_config c = {0};
        if (op == WIFI_CONNECT) c.network = *(struct wifi_request *)data;
        else c = *(struct wifi_config *)data;
        int rv = associate(&c); memset(&c, 0, sizeof(c));
        if (rv) { disconnect(); wifi.state = WIFI_FAILED; }
        wifi.error = rv; return rv;
    }
    return GZ_EINVAL;
}
bool wl54cf_init(void)
{
    wl_set_cor(0x80);
    wl_set_cor(1); /* PC Card COR reset / configuration index from CIS. */
    wl_write16(4, 31);
    /* Product revision (Linux libertas if_cs: IF_CS_PRODUCT_ID): the
     * CF8385 is supported from B1 (0x12) on; later revisions report more. */
    /* A card just configured may not answer at once (real cards read 0
     * at first): wait for the same revision twice. */
    uint8_t rev = 0, last = 0xff;
    for (uint64_t t0 = time_us(); time_us() - t0 < 1000000;) {
        rev = wl_read8(0x1c);
        if (rev && rev != 0xff && rev == last)
            break;
        last = rev;
        for (uint64_t t1 = time_us(); time_us() - t1 < 10000;)
            ;
    }
    uint8_t state = wl_read8(0x3f);
    kprintf("wl54cf: revision %x, firmware state %x\n", rev, state);
    /* Linux libertas refuses revisions below B1 (0x12) with its own
     * firmware; a real WL54-CF is B0 (0x10), and the firmware here comes
     * from the card's own Windows CE driver (docs/WL54CF.md), so B0 is
     * tried as well. */
    if (rev < 0x10 || rev == 0xff) {
        kputs("wl54cf: unsupported chip revision; I/O registers:");
        for (unsigned r = 0; r < 0x40; r++)
            kprintf("%s%02x", r % 16 ? " " : "\n  ", wl_read8(r));
        kputs("\n");
        goto fail;
    }
    if (!firmware()) {
        kprintf("wl54cf: firmware download failed (state %x, scratch %x)\n", wl_read8(0x3f),
                wl_read16(0x28));
        goto fail;
    }
    wl_write16(0x22, 31);
    memset(cmd, 0, 46);
    if (command(3, 46) != 46)
        goto fail;
    /* A real B0 card leaves the permanent address zero here: its MAC is the
     * CIS's LAN_NODE_ID (CISTPL_FUNCE), as its Windows CE driver reads it. */
    static const uint8_t zero[6];
    uint8_t mac[6];
    memcpy(mac, buf + 16, 6);
    if (!memcmp(mac, zero, 6) && pcmcia_node_id(mac)) {
        /* The firmware sends from the address it holds: give it this one
         * (CMD_802_11_MAC_ADDRESS, action SET, as Linux libertas
         * lbs_set_mac_address), or the access point sees 00:00:00:00:00:00
         * and authentication times out (association status 4). */
        memset(cmd, 0, 16);
        put16(cmd + 8, 1);
        memcpy(cmd + 10, mac, 6);
        unsigned set = command(0x4d, 16);
        kprintf("wl54cf: station address from the CIS%s\n", set ? ", set in the firmware" : "; the firmware refused it");
    }
    net_set_mac(mac);
    kprintf("wl54cf: MAC %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3],
            mac[4], mac[5]);
    memset(cmd, 0, 12);
    put16(cmd + 8, 1);
    put16(cmd + 10, 1);
    if (!command(0x1c, 12)) goto fail;
    memset(&wifi, 0, sizeof(wifi)); wifi.state = WIFI_DISCONNECTED;
    wifi.capabilities = WIFI_CAP(WIFI_OPEN) | WIFI_CAP(WIFI_WEP) | WIFI_CAP(WIFI_WPA_PSK) | WIFI_CAP(WIFI_WPA2_PSK);
    if (clear_keys()) goto fail;
    wl_write16(0x22, 31);
    wl_write16(4, 31 & ~(RX | EVENT));
    online = true;
    KJ_SRC_PCMCIA |= KJ_SRC_ENABLE;
    kputs("wl54cf: ready (disconnected)\n");
    return true;
fail:
    wl_write16(4, 31);
    kputs("wl54cf: initialization failed\n");
    return false;
}
void wl54cf_irq(void)
{
    if (!online)
        return;
    for (unsigned budget = 0; budget < 8; budget++) {
        uint16_t cause = wl_read16(0x22);
        if (cause == 0xffff) {
            online = false;
            return;
        }
        if (!(cause & (RX | EVENT)))
            return;
        if (cause & RX) {
            unsigned n = wl_read16(0x24);
            if (n >= 20 && n <= sizeof(buf)) {
                read_fifo(0x10, buf, n);
                uint32_t off = get32(buf + 8), len = get16(buf + 4);
                if ((get16(buf) & 1) && off >= 20 && off <= n && len >= 22 && len <= n - off &&
                    len - 8 <= GZ_ETH_FRAME_MAX &&
                    !memcmp(buf + off + 14, "\xaa\xaa\x03\0\0\0", 6)) {
                    /* Strip the length and SNAP prefix; retain dst/src/type. */
                    memmove(buf + off + 8, buf + off, 12);
                    uint8_t *frame = buf + off + 8;
                    if (authorized || (frame[12] == 0x88 && frame[13] == 0x8e))
                        net_receive(frame, len - 8);
                    else net_rx_drop();
                } else {
                    /* DEBUG(hw): what a real card's receive descriptor holds. */
                    static unsigned logged;
                    if (logged++ < 8)
                        kprintf("wl54cf: rx drop: %u bytes, status %x len %u off %u: %02x %02x %02x %02x %02x %02x\n",
                                n, get16(buf), len, off, off < n ? buf[off] : 0,
                                off + 12 < n ? buf[off + 12] : 0, off + 13 < n ? buf[off + 13] : 0,
                                off + 14 < n ? buf[off + 14] : 0, off + 15 < n ? buf[off + 15] : 0,
                                off + 16 < n ? buf[off + 16] : 0);
                    net_rx_drop();
                }
            } else {
                kprintf("wl54cf: rx length %u\n", n);
                net_rx_drop();
            }
            signal(RX);
        }
        if (cause & EVENT) {
            unsigned event = (wl_read16(0x20) >> 8) & 0x7f;
            signal(EVENT);
            if (event == 3) {
                authorized = pairwise_key = group_key = false;
                wifi.state = WIFI_FAILED; wifi.error = GZ_ENETDOWN;
                memset(&wifi.current, 0, sizeof(wifi.current));
                kputs("wl54cf: link lost\n");
            }
        }
        wl_write16(0x22, cause & (RX | EVENT));
    }
}
int wl54cf_send(const uint8_t *frame, uint32_t len)
{
    if (!online || (wifi.state != WIFI_CONNECTED && wifi.state != WIFI_AUTHENTICATING) ||
        (!authorized && (len < 14 || frame[12] != 0x88 || frame[13] != 0x8e))) return GZ_ENETDOWN;
    if (len < 14 || len > GZ_ETH_FRAME_MAX || !wait_bits(0x20, TX))
        return GZ_EIO;
    memset(buf, 0, 24);
    put32(buf + 8, 24);
    put16(buf + 12, len);
    memcpy(buf + 14, frame, 6);
    memcpy(buf + 24, frame, len);
    wl_write16(0x22, TX);
    wl_write16(0x14, 24 + len);
    write_fifo(0x16, buf, 24 + len);
    signal(TX);
    if (!wait_bits(0x22, TX))
        return GZ_EIO;
    wl_write16(0x22, TX);
    return (int)len;
}

/* The card was pulled: stop using it; a card inserted later starts over. */
void wl54cf_removed(void)
{
    KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE;
    online = authorized = pairwise_key = group_key = false;
    memset(&wifi, 0, sizeof(wifi));
    memset(cmd, 0, sizeof(cmd)); memset(buf, 0, sizeof(buf));
}
