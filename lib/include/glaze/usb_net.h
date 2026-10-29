/* Ethernet tunnel over the existing BE-300 USB bulk endpoints. */
#ifndef GLAZE_USB_NET_H
#define GLAZE_USB_NET_H
#define GUN_MTU 1500u
#define GUN_FRAME_MAX 1514u
#define GUN_QUEUE 8u
#define GUN_DEVICE_MAC {2, 0x47, 0x4c, 0x41, 0x5a, 2}
#define GUN_HOST_MAC   {2, 0x47, 0x4c, 0x41, 0x5a, 1}
#define GUN_HOST_IP "192.168.7.1"
#define GUN_DEVICE_IP "192.168.7.2"
#define GUN_MASK "255.255.255.252"
#define GUN_DNS "1.1.1.1"
#endif
