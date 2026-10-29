#include "ethernet_packet.h"
#include <glaze/usb_net.h>
#include <string.h>
static const uint8_t host_mac[6] = GUN_HOST_MAC, device_mac[6] = GUN_DEVICE_MAC;
static const uint8_t host_ip[4] = {192,168,7,1}, device_ip[4] = {192,168,7,2};
static unsigned ip_size(const uint8_t *p, unsigned size)
{
    if (size < 20 || (p[0] >> 4) != 4) return 0;
    unsigned header = (p[0] & 15) * 4, total = (unsigned)p[2] * 256 + p[3];
    return header >= 20 && total >= header && total <= GUN_MTU && total <= size ? total : 0;
}
unsigned gu_eth_from_ip(uint8_t *out, const uint8_t *in, unsigned size)
{
    if (size < 4 || in[0] || in[1] || in[2] || in[3] != 2) return 0;
    unsigned n = ip_size(in + 4, size - 4);
    if (!n) return 0;
    memcpy(out, device_mac, 6); memcpy(out + 6, host_mac, 6);
    out[12] = 8; out[13] = 0; memcpy(out + 14, in + 4, n);
    return n + 14;
}
unsigned gu_eth_to_ip(uint8_t *out, const uint8_t *in, unsigned size,
                      uint8_t *arp, unsigned *arp_size)
{
    *arp_size = 0;
    if (size < 14 || size > GUN_FRAME_MAX) return 0;
    if (in[12] == 8 && in[13] == 0) {
        unsigned n = ip_size(in + 14, size - 14);
        if (!n) return 0;
        memset(out, 0, 4); out[3] = 2; memcpy(out + 4, in + 14, n);
        return n + 4;
    }
    if (in[12] != 8 || in[13] != 6 || size < 42) return 0;
    const uint8_t *a = in + 14;
    static const uint8_t request[8] = {0,1,8,0,6,4,0,1};
    if (memcmp(a,request,8) || memcmp(a+8,device_mac,6) || memcmp(in+6,device_mac,6) ||
        memcmp(a+14,device_ip,4) || memcmp(a+24,host_ip,4)) return 0;
    memset(arp,0,60);
    memcpy(arp,device_mac,6); memcpy(arp+6,host_mac,6); arp[12]=8; arp[13]=6;
    memcpy(arp+14,request,8); arp[21]=2;
    memcpy(arp+22,host_mac,6); memcpy(arp+28,host_ip,4);
    memcpy(arp+32,device_mac,6); memcpy(arp+38,device_ip,4);
    *arp_size=60; return 0;
}
