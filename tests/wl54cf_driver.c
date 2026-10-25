/* Production Glaze driver vs the independently sourced BE300 card model. */
#include "../kernel/drivers.h"
#include "../kernel/kernel.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define wl54cf_init wl54cf_model_init
#include "hw/wl54cf.h"
#undef wl54cf_init
#define GLAZE_HW_H
#define GLAZE_WL54CF_IO_H
static uint32_t source_enable;
#define KJ_SRC_PCMCIA source_enable
#define KJ_SRC_ENABLE 8
static wl54cf_t card;
static unsigned mode, drops, frames;
static uint64_t clock_us;
static uint8_t received[1600], station_mac[6];
static size_t received_len;
static uint8_t wl_read8(unsigned r) { return (r == 0x1c && mode == 8) ? 0x0f : wl54cf_read(&card, r, 1); }
static uint16_t wl_read16(unsigned r)
{
    if (r == 0x28 && mode == 7)
        return 65534;
    if (r == 0x22 && mode == 2)
        return 0; /* command response timeout */
    return wl54cf_read(&card, r, 2);
}
static void wl_write8(unsigned r, uint8_t v) { wl54cf_write(&card, r, 1, v); }
static void wl_write16(unsigned r, uint16_t v)
{
    wl54cf_write(&card, r, 2, v);
    if (r == 2 && (v & 4) && card.firmware_stage == 2 && card.response.length) {
        if (mode == 3)
            card.response.data[4] ^= 1; /* stale sequence */
        if (mode == 4)
            card.response.length = 65535; /* invalid FIFO size */
        if (mode == 5)
            card.response.data[2] = 0xff; /* header past FIFO */
        if (mode == 6 && card.response.data[0] == 6)
            card.response.data[11] = 0xff; /* truncated beacon */
    }
}
static void wl_set_cor(uint8_t v)
{
    if (v & 0x80)
        wl54cf_reset(&card);
}
#include "../kernel/drivers/wl54cf.c"
static struct romfs_entry helper, firmware_image;
static uint8_t helper_data[256], firmware_data[34];
const struct romfs_entry *romfs_lookup(const char *name)
{
    if (mode == 1)
        return NULL;
    return strstr(name, "helper") ? &helper : &firmware_image;
}
int romfs_read(const struct romfs_entry *e, uint32_t off, void *p, uint32_t n)
{
    if (off > e->size || n > e->size - off)
        return -1;
    memcpy(p, (e == &helper ? helper_data : firmware_data) + off, n);
    return 0;
}
bool pcmcia_node_id(uint8_t mac[6]) { (void)mac; return false; }
uint64_t time_us(void)
{
    clock_us += 1000;
    return clock_us;
}
void kputs(const char *s) {}
void kprintf(const char *fmt, ...) {}
void net_set_mac(const uint8_t p[6]) { memcpy(station_mac, p, 6); }
void net_rx_drop(void) { drops++; }
void net_receive(const uint8_t *p, uint32_t n)
{
    frames++;
    received_len = n;
    memcpy(received, p, n);
}
static void initialize(unsigned fault)
{
    static const uint8_t mac[6] = {2, 3, 4, 5, 6, 7};
    wl54cf_model_init(&card, mac);
    mode = fault;
    online = false;
    sequence = 0;
    clock_us = 0;
    drops = frames = 0;
    helper.size = sizeof(helper_data);
    firmware_image.size = sizeof(firmware_data);
    memset(firmware_data, 0, sizeof(firmware_data));
    firmware_data[0] = 1;
    firmware_data[8] = 2;
    firmware_data[18] = 4;
}
int main(void)
{
    /* The guest driver and emulator expose differently named init entrypoints
     * via the compile define on this translation unit. */
    for (unsigned fault = 1; fault <= 8; fault++) {
        initialize(fault);
        if (fault == 6) continue;
        assert(!wl54cf_init());
        assert(!online);
        assert(card.mask == 31); /* failure never leaves an IRQ source active */
    }
    initialize(0);
    assert(wl54cf_init());
    assert(!card.associated && online);
    mode=6;assert(wl54cf_control(WIFI_SCAN, NULL)==GZ_EIO);mode=0;
    struct wifi_request request={.ssid_len=5};memcpy(request.ssid,"BE300",5);
    assert(!wl54cf_control(WIFI_CONNECT,&request));
    assert(card.associated && wifi.state==WIFI_CONNECTED);
    assert(!memcmp(station_mac, card.mac, 6));
    uint8_t packet[61] = {0};
    memcpy(packet, card.mac, 6);
    packet[12] = 8;
    packet[60] = 0x6d;
    assert(wl54cf_receive(&card, packet, sizeof(packet)));
    wl54cf_irq();
    assert(frames == 1 && received_len == 61 && !memcmp(received, packet, 61));
    assert(wl54cf_receive(&card, packet, sizeof(packet)));
    card.rx[card.rx_head].data[8] = 0xff;
    wl54cf_irq();
    assert(drops == 1 && frames == 1);
    assert(!card.rx_count);
    assert(wl54cf_send(packet, sizeof(packet)) == 61);
    assert(!wl54cf_irq_pending(&card));
    assert(!wl54cf_control(WIFI_DISCONNECT,NULL));
    assert(!authorized && !card.security.authorized);
    assert(wifi_security_configure(&card.security,WIFI_WEP,"12345"));
    struct wifi_config config={0};config.network=request;config.security=WIFI_WEP;
    config.key_len=5;memcpy(config.key,"12345",5);
    for(unsigned slot=0;slot<4;slot++) {
        config.key_index=slot;assert(!wl54cf_control(WIFI_CONFIGURE,&config));
        assert(wifi.state==WIFI_CONNECTED && card.security.authorized);
        assert(!wl54cf_control(WIFI_DISCONNECT,NULL));
    }
    assert(wifi_security_configure(&card.security,WIFI_WEP,"1234567890123"));
    config.key_len=13;memcpy(config.key,"1234567890123",13);
    assert(!wl54cf_control(WIFI_CONFIGURE,&config));
    assert(wifi.state==WIFI_CONNECTED);
    assert(!wl54cf_control(WIFI_DISCONNECT,NULL));
    config.key[0]^=1;assert(wl54cf_control(WIFI_CONFIGURE,&config)==WIFI_EAUTH);
    assert(!authorized && !card.security.authorized);
    puts("wl54cf driver: open/WEP, slots, wrong key, disconnect and malformed firmware PASS");
    return 0;
}
