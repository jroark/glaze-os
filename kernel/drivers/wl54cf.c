/* Marvell CF8385 host driver. Protocol references: docs/WL54CF.md.
 * All integers on the wire are explicitly little endian; no unaligned loads.
 * Firmware commands run synchronously during boot. Runtime IRQs move frames.
 */
#include "../drivers.h"
#include "../hw.h"
#include "../kernel.h"

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
static const uint8_t ap[6] = {2, 0x42, 0x45, 0x33, 0x30, 0x30};
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
    if (n < 8 || n > sizeof(cmd) || !wait_bits(0x20, COMMAND))
        return 0;
    put16(cmd, op);
    put16(cmd + 2, n);
    put16(cmd + 4, ++sequence);
    put16(cmd + 6, 0);
    wl_write16(0x18, n);
    write_fifo(0x1a, cmd, n);
    signal(COMMAND);
    if (!wait_bits(0x22, RESPONSE))
        return 0;
    unsigned count = wl_read16(0x30);
    if (count < 16 || count > sizeof(buf)) {
        wl_write16(0x22, RESPONSE);
        return 0;
    }
    read_fifo(0x12, buf, count);
    wl_write16(0x22, RESPONSE);
    unsigned length = get16(buf + 2);
    if (get16(buf) != (op | 0x8000) || get16(buf + 4) != sequence || get16(buf + 6) || length < 8 ||
        length > count - 8)
        return 0;
    return length;
}
/* Parse standard SCAN response, including all bounds before dereferencing IEs. */
static bool scan(void)
{
    memset(cmd, 0, sizeof(cmd));
    cmd[8] = 3;
    put16(cmd + 15, 0);
    put16(cmd + 17, 5);
    memcpy(cmd + 19, "BE300", 5);
    unsigned n = command(6, 24);
    if (n < 11 || get16(buf + 8) > n - 11)
        return false;
    unsigned end = 11 + get16(buf + 8), pos = 11;
    for (unsigned b = 0; b < buf[10]; b++) {
        if (pos + 2 > end)
            return false;
        unsigned length = get16(buf + pos);
        pos += 2;
        if (length < 19 || length > end - pos)
            return false;
        unsigned stop = pos + length;
        bool match = !memcmp(buf + pos, ap, 6) && (get16(buf + pos + 17) & 0x11) == 1;
        bool ssid = false, channel = false;
        for (unsigned ie = pos + 19; ie + 2 <= stop;) {
            unsigned tag = buf[ie], l = buf[ie + 1];
            ie += 2;
            if (l > stop - ie)
                return false;
            if (!tag && l == 5 && !memcmp(buf + ie, "BE300", 5))
                ssid = true;
            if (tag == 3 && l == 1 && buf[ie] == 3)
                channel = true;
            ie += l;
        }
        if (match && ssid && channel)
            return true;
        pos = stop;
    }
    return false;
}
bool wl54cf_init(void)
{
    wl_set_cor(0x80);
    wl_set_cor(1); /* PC Card COR reset / configuration index from CIS. */
    wl_write16(4, 31);
    if (wl_read8(0x1c) != 0x12 || !firmware())
        goto fail;
    wl_write16(0x22, 31);
    memset(cmd, 0, 46);
    if (command(3, 46) != 46)
        goto fail;
    net_set_mac(buf + 16);
    kprintf("wl54cf: MAC %02x:%02x:%02x:%02x:%02x:%02x\n", buf[16], buf[17], buf[18], buf[19],
            buf[20], buf[21]);
    memset(cmd, 0, 12);
    put16(cmd + 8, 1);
    put16(cmd + 10, 1);
    if (!command(0x1c, 12) || !scan())
        goto fail;
    memset(cmd, 0, 25);
    memcpy(cmd + 8, ap, 6);
    if (!command(0x11, 25))
        goto fail;
    memset(cmd, 0, sizeof(cmd));
    memcpy(cmd + 8, ap, 6);
    put16(cmd + 14, 1);
    put16(cmd + 16, 10);
    put16(cmd + 18, 100);
    unsigned p = 21;
    put16(cmd + p, 0);
    put16(cmd + p + 2, 5);
    memcpy(cmd + p + 4, "BE300", 5);
    p += 9;
    put16(cmd + p, 3);
    put16(cmd + p + 2, 1);
    cmd[p + 4] = 3;
    p += 5;
    put16(cmd + p, 4);
    put16(cmd + p + 2, 6);
    p += 10;
    static const uint8_t rates[] = {0x82, 0x84, 0x8b, 0x96, 12, 18, 24, 36, 48, 72, 96, 108};
    put16(cmd + p, 1);
    put16(cmd + p + 2, sizeof(rates));
    memcpy(cmd + p + 4, rates, sizeof(rates));
    p += 4 + sizeof(rates);
    if (command(0x50, p) < 14 || get16(buf + 10))
        goto fail;
    memset(cmd, 0, 12);
    put16(cmd + 8, 0x43); /* RX, TX, broadcast (Libertas host.h). */
    if (!command(0x28, 12))
        goto fail;
    wl_write16(0x22, 31);
    wl_write16(4, 31 & ~(RX | EVENT));
    online = true;
    KJ_SRC_PCMCIA |= KJ_SRC_ENABLE;
    kputs("wl54cf: associated BE300 channel 3\n");
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
                    net_receive(buf + off + 8, len - 8);
                } else
                    net_rx_drop();
            } else
                net_rx_drop();
            signal(RX);
        }
        if (cause & EVENT) {
            unsigned event = (wl_read16(0x20) >> 8) & 0x7f;
            signal(EVENT);
            if (event == 3) {
                online = false;
                kputs("wl54cf: link lost\n");
            }
        }
        wl_write16(0x22, cause & (RX | EVENT));
    }
}
/* The card was pulled: stop using it; a card inserted later starts over. */
void wl54cf_removed(void)
{
    KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE;
    online = false;
}
int wl54cf_send(const uint8_t *frame, uint32_t len)
{
    if (!online || len < 14 || len > GZ_ETH_FRAME_MAX || !wait_bits(0x20, TX))
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
