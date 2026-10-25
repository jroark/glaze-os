/* Exercise the production driver against the emulator's PRISM device model.
 * Build with XI825_MODEL pointing at the emulator source checkout. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <glaze/abi.h>
#define xi825_init prism_init
#include "xi825.h"
#undef xi825_init
#define PROC_NET 4
struct proc { unsigned flags; int as; };
static xi825_t model;
static uint64_t now;
static bool tick = true, stall;
static uint8_t user[8192], sent[1600];
static unsigned sent_len, notifications;
static uint64_t time_us(void) { now += 100; return now; }
static uint16_t test_read(unsigned off)
{
    if (tick) xi825_tick(&model);
    if (stall && !off) return 0x8000;
    return xi825_read(&model, off, 2);
}
static void test_write(unsigned off, unsigned v) { xi825_write(&model, off, 2, v); }
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t v) { (void)v; }
static int copy_to_as(int *as, uint32_t out, const void *src, unsigned n)
{
    (void)as;
    if (out > sizeof(user) || n > sizeof(user) - out) return GZ_EINVAL;
    memcpy(user + out, src, n); return 0;
}
static int copy_from_as(int *as, void *out, uint32_t src, unsigned n)
{
    (void)as;
    if (src > sizeof(user) || n > sizeof(user) - src) return GZ_EINVAL;
    memcpy(out, user + src, n); return 0;
}
static void ipc_notify(int port, unsigned bits) { (void)port; (void)bits; notifications++; }
static void kprintf(const char *fmt, ...) { (void)fmt; }
#define XI825_TEST
#define xread(r) test_read(r)
#define xwrite(r,v) test_write(r,v)
#define xread8(r) xi825_read(&model, r, 1)
#define xwrite8(r,v) xi825_write(&model, r, 1, v)
#define xcor(v) ((void)(v))
#define xenable() ((void)0)
#define xdisable() ((void)0)
#include "../kernel/drivers/xi825.c"
#define CHECK(v) do { if (!(v)) { fprintf(stderr, "line %d: %s\n", __LINE__, #v); exit(1); } } while (0)
static void transmit(void *opaque, const uint8_t *data, size_t len)
{
    (void)opaque; CHECK(len <= sizeof(sent)); memcpy(sent, data, len); sent_len = len;
}
int main(void)
{
    const uint8_t mac[6] = {2,3,4,5,6,7};
    struct proc p = { .flags = PROC_NET }, unpriv = {0};
    prism_init(&model, mac);
    model.transmit = transmit;
    xi825_init();
    CHECK(present && !model.enabled && wifi.state == WIFI_DISCONNECTED);
    CHECK(!memcmp(nic.mac, mac, 6));
    CHECK(xi825_bind(&p, 1) == 0);
    CHECK(xi825_control(&unpriv, WIFI_SCAN, 0) == GZ_EPERM);
    CHECK(xi825_info(&unpriv, 0) == GZ_EPERM);
    CHECK(xi825_send(&unpriv, 0, 64) == GZ_EPERM);
    CHECK(xi825_control(&p, WIFI_STATUS, 9000) == GZ_EINVAL);
    CHECK(xi825_control(&p, WIFI_SCAN, 0) == 0);
    xi825_irq();
    CHECK(!wifi.scanning && wifi.count == 1);
    CHECK(wifi.aps[0].ssid_len == 5 && !memcmp(wifi.aps[0].ssid, "BE300", 5));
    CHECK(wifi.aps[0].channel == 3 && !wifi.aps[0].privacy);
    struct wifi_request req = { .ssid_len = 33 };
    memcpy(user, &req, sizeof(req));
    CHECK(xi825_control(&p, WIFI_CONNECT, 0) == GZ_EINVAL);
    req.ssid_len = 5; memcpy(req.ssid, "BE300", 5); memcpy(user, &req, sizeof(req));
    CHECK(xi825_control(&p, WIFI_CONNECT, 0) == 0);
    xi825_irq(); CHECK(wifi.state == WIFI_CONNECTED);
    CHECK(xi825_control(&p, WIFI_STATUS, 0) == 0);
    CHECK(wifi.current.rate == 22 && wifi.current.channel == 3);
    CHECK(wifi.current.ssid_len == 5);
    /* Odd-sized DNS-like Ethernet frame: TX then the reverse RX path. */
    uint8_t packet[79];
    memcpy(packet, mac, 6); memcpy(packet + 6, "source", 6);
    packet[12] = 8; packet[13] = 0;
    for (unsigned i = 14; i < sizeof(packet); i++) packet[i] = i;
    memcpy(user, packet, sizeof(packet));
    for (unsigned i = 0; i < 30; i++) {
        CHECK(xi825_send(&p, 0, sizeof(packet)) == sizeof(packet));
        CHECK(sent_len == sizeof(packet) && !memcmp(sent, packet, sent_len));
        CHECK(xi825_receive(&model, packet, sizeof(packet)));
        xi825_irq();
        CHECK(xi825_recv(&p, 512, 1536) == sizeof(packet));
        CHECK(!memcmp(user + 512, packet, sizeof(packet)));
    }
    const unsigned lengths[] = {14, 60, 1513, 1514};
    uint8_t large[GZ_ETH_FRAME_MAX];
    memcpy(large, packet, sizeof(packet));
    for (unsigned i = sizeof(packet); i < sizeof(large); i++) large[i] = i;
    for (unsigned i = 0; i < sizeof(lengths)/sizeof(lengths[0]); i++) {
        unsigned n = lengths[i];
        memcpy(user, large, n);
        CHECK(xi825_send(&p, 0, n) == (int)n);
        CHECK(sent_len == n && !memcmp(sent, large, n));
        CHECK(xi825_receive(&model, large, n)); xi825_irq();
        CHECK(xi825_recv(&p, 2048, 1536) == (int)n && !memcmp(user + 2048, large, n));
    }
    CHECK(xi825_send(&p, 0, 1515) == GZ_EINVAL);
    CHECK(xi825_send(&p, sizeof(user)-5, 64) == GZ_EINVAL);
    for (unsigned i = 0; i < 10; i++) { CHECK(xi825_receive(&model, packet, sizeof(packet))); xi825_irq(); }
    CHECK(nic.rx_dropped == 2);
    for (unsigned i = 0; i < 8; i++) CHECK(xi825_recv(&p, 512, 1536) == sizeof(packet));
    CHECK(xi825_recv(&p, 512, 1536) == 0);
    CHECK(xi825_receive(&model, packet, sizeof(packet))); xi825_irq();
    CHECK(head != tail);
    model.enabled = false; xi825_tick(&model); xi825_irq();
    CHECK(wifi.state == WIFI_FAILED && head == tail && !wifi.current.ssid_len);
    CHECK(xi825_control(&p, WIFI_DISCONNECT, 0) == 0);
    CHECK(wifi.state == WIFI_DISCONNECTED && !wifi.current.ssid_len);
    CHECK(xi825_send(&p, 0, sizeof(packet)) == GZ_ENETDOWN);
    /* Unavailable saved network must time out and stay down. */
    req.ssid_len = 7; memcpy(req.ssid, "missing", 7); memcpy(user, &req, sizeof(req));
    CHECK(xi825_control(&p, WIFI_CONNECT, 0) == 0);
    now += 11000000; xi825_irq();
    CHECK(wifi.state == WIFI_FAILED && wifi.error == GZ_ETIMEDOUT && !model.enabled);
    uint8_t badscan[5] = {0};
    scan_result(badscan, sizeof(badscan)); CHECK(wifi.error == GZ_EIO);
    CHECK(bap(tx_fid, 4094, scratch, 4, false) == GZ_EINVAL);
    CHECK(bap(0xdead, 0, scratch, 2, false) == GZ_EIO);
    CHECK(command(0x3f, 0) == GZ_EIO);
    stall = true; CHECK(command(1, 0) == GZ_ETIMEDOUT); stall = false;
    tick = false;
    CHECK(xi825_control(&p, WIFI_SCAN, 0) == 0);
    CHECK(xi825_control(&p, 99, 0) == GZ_EINVAL);
    CHECK(notifications > 0);
    puts("xi825: firmware commands, association, scan, odd TX/RX, reclaim, queue bounds, permissions and timeouts PASS");
}
