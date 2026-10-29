#ifndef HOST_ETHERNET_PACKET_H
#define HOST_ETHERNET_PACKET_H
#include <stdint.h>
/* Darwin utun packets have a network-order AF_INET (2) prefix. Invalid,
 * oversized and IPv6 packets are ignored. Ethernet padding is stripped.
 * to_ip may produce a 60-byte ARP response instead (arp_size = 60). */
unsigned gu_eth_from_ip(uint8_t *ethernet, const uint8_t *utun, unsigned size);
unsigned gu_eth_to_ip(uint8_t *utun, const uint8_t *ethernet, unsigned size,
                      uint8_t *arp, unsigned *arp_size);
#endif
