#include "../host/glaze_usb/pcconnect.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t received[1024], written[1024];
static unsigned count, position, written_len, chunk = 1;
volatile sig_atomic_t usb_cancelled;
int usb_write(struct usb_link *l, const void *p, size_t n, unsigned timeout)
{ (void)l; (void)timeout; assert(n <= sizeof(written)); memcpy(written, p, n); written_len = (unsigned)n; return 0; }
int usb_read(struct usb_link *l, void *p, size_t n, unsigned timeout)
{
    (void)l; (void)timeout;
    assert(position < count);
    if (n > count - position) n = count - position;
    if (n > chunk) n = chunk;
    memcpy(p, received + position, n); position += (unsigned)n; return (int)n;
}
int usb_lines(struct usb_link *l, int n) { (void)l; (void)n; return 0; }
int usb_open(struct usb_link *l, const char *path, int n) { (void)l; (void)path; (void)n; return 0; }
void usb_close(struct usb_link *l) { (void)l; }
static void reply(unsigned seq, unsigned op)
{
    memset(received, 0, sizeof(received)); position = 0;
    received[0] = 0x55; received[1] = 0x55; received[2] = 0x55;
    uint8_t *b = received + 3; b[0] = 0x11; b[1] = (uint8_t)seq;
    pc_put16(b + 2, 22); pc_put16(b + 4, op); pc_put32(b + 10, 1); pc_put32(b + 18, 0);
    pc_checksums(b, 22); count = 25;
}
int main(void)
{
    static const uint8_t query[] = {0x10,1,0,10,3,0x2d,0,0,0,0x4b};
    uint8_t q[10]; memcpy(q, query, 10); pc_checksums(q, 10);
    assert(!memcmp(q, query, 10) && pc_valid(q, 10));
    q[8] ^= 1; assert(!pc_valid(q, 10));
    static const uint8_t answer[] = {0x11,1,0,0x12,3,0x2d,0,0,0,0x54,0,0,0,0,0,0,0,0};
    assert(pc_valid(answer, sizeof(answer)));
    uint8_t path[100];
    assert(pc_path(path, sizeof(path), "A") == 6);
    assert(!memcmp(path, "\0\4\0A\0\0", 6));
    assert(pc_path(path, 5, "A") < 0 && pc_path(path, sizeof(path), "\xc3\xa9") < 0);
    for (chunk = 1; chunk < 32; chunk++) {
        struct pc_link p = {0}; uint8_t request[8];
        pc_put32(request, 17); pc_put32(request + 4, 4096);
        reply(1, 0x0319);
        assert(pc_call(&p, 0x0319, request, sizeof(request)) == 22);
        assert(p.result == 1 && p.error == 0 && written_len == 18 && pc_valid(written, written_len));
        assert(pc_be32(written + 10) == 17 && pc_be32(written + 14) == 4096);
    }
    struct pc_link p = {0}; reply(1, 0x0319); received[count - 1] ^= 1;
    assert(pc_call(&p, 0x0319, NULL, 0) == LIBUSB_ERROR_IO);
    /* Some ROM versions echo the request's checksum fields in replies. */
    memset(&p, 0, sizeof(p)); reply(1, 0x0319);
    uint8_t legacy_request[10] = {0x10, 1, 0, 10, 3, 0x19};
    pc_checksums(legacy_request, sizeof(legacy_request));
    memcpy(received + 3 + 6, legacy_request + 6, 4);
    assert(!pc_valid(received + 3, 22));
    assert(pc_call(&p, 0x0319, NULL, 0) == 22 && p.result == 1);
    unsigned old_position = position;
    assert(!pc_reboot(&p));
    assert(written_len == 10 && pc_valid(written, 10) && pc_be16(written + 4) == 0x032a);
    assert(position == old_position); /* SoftReset has no response to read. */
    puts("usb-pcconnect: ALL PASS"); return 0;
}
