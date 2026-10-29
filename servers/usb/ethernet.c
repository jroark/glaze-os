/* Independent USB Ethernet session. The debugger may use paired TLS over
 * this very link while this parser pumps physical USB, without recursive
 * dispatch, shared replay state or plaintext debugger commands in the tunnel.
 */
#include "ethernet.h"
#include <glaze/usb.h>
#include <glaze/usb_net.h>
#include <string.h>
#if !defined(__mips__)
extern uint32_t gz_time_ms(void);
extern void gz_sleep_ms(uint32_t);
extern int gz_usb_ctl(uint32_t, void *, uint32_t);
#endif

static struct gu_parser parser;
static uint8_t frame[GU_HEADER + 4 + GUN_FRAME_MAX], body[4 + GUN_FRAME_MAX];
static uint32_t connection, activity, last_id, last_op, last_crc, last_len;
static int last_status;
static bool active, have_connection;

int usb_ethernet_active(void) { return active; }
int usb_ethernet_open(uint32_t id, uint32_t crc)
{
    int r = gz_usb_ctl(GZ_USB_NET_OPEN, NULL, 0);
    if (r) return r;
    active = true; have_connection = false; parser.used = 0;
    activity = gz_time_ms();
    last_id = id; last_op = GU_USB_NET_OPEN; last_crc = crc;
    last_status = last_len = 0;
    return 0;
}
static int reply(uint32_t op, uint32_t id, int status, unsigned n)
{
    unsigned size = gu_encode(frame, op | GU_REPLY, id, status, body, n), off = 0;
    uint32_t end = gz_time_ms() + 3000;
    while (off < size) {
        int r = gz_usb_ctl(GZ_USB_WRITE, frame + off, size - off);
        if (r < 0) return r;
        off += r;
        if ((int32_t)(gz_time_ms() - end) >= 0) return GZ_ETIMEDOUT;
        if (!r) gz_sleep_ms(1);
    }
    return 0;
}
static void close_link(void)
{
    gz_usb_ctl(GZ_USB_NET_CLOSE, NULL, 0);
    active = false; parser.used = 0;
}
void usb_ethernet_poll(uint32_t state)
{
    if (!active) return;
    if (!have_connection) { connection = state; have_connection = true; }
    if (state != connection || (state & 3) != 3 || gz_time_ms() - activity >= 10000) {
        close_link(); return;
    }
    uint8_t bytes[256];
    int n = gz_usb_ctl(GZ_USB_READ, bytes, sizeof(bytes));
    if (n < 0) { close_link(); return; }
    for (int i = 0; i < n; i++) {
        if (gu_feed(&parser, bytes[i]) != 1) continue;
        uint32_t op = gu_get32(parser.bytes + 4) >> 16;
        uint32_t id = gu_get32(parser.bytes + 8), len = gu_get32(parser.bytes + 12);
        uint32_t crc = gu_get32(parser.bytes + 20);
        if (!id || (op & GU_REPLY) || gu_get32(parser.bytes + 16)) continue;
        activity = gz_time_ms();
        if (id == last_id && op == last_op && crc == last_crc) {
            if (reply(op, id, last_status, last_len)) { close_link(); return; }
            continue;
        }
        int status = GZ_EINVAL; unsigned size = 0;
        if (id > last_id && op == GU_USB_NET_EXCHANGE && (!len || (len >= 14 && len <= GUN_FRAME_MAX))) {
            memcpy(body, parser.bytes + GU_HEADER, len);
            int r = gz_usb_ctl(GZ_USB_NET_EXCHANGE, body, len);
            if (r >= 4 && r <= (int)sizeof(body)) { size = r; status = 0; }
            else status = r < 0 ? r : GZ_EIO;
        } else if (id > last_id && op == GU_USB_NET_CLOSE && !len) {
            status = 0;
        }
        /* Invalid/out-of-order requests must not reuse an earlier result. */
        if (id > last_id) {
            last_id = id; last_op = op; last_crc = crc;
            last_status = status; last_len = size;
        }
        if (reply(op, id, status, size) || (!status && op == GU_USB_NET_CLOSE)) {
            close_link(); return;
        }
    }
}
