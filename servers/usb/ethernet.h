#ifndef USB_ETHERNET_H
#define USB_ETHERNET_H
#include <stdint.h>
int usb_ethernet_open(uint32_t id, uint32_t crc);
int usb_ethernet_active(void);
void usb_ethernet_poll(uint32_t connection);
#endif
