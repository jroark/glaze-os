#ifndef GLAZE_USB_TRANSPORT_H
#define GLAZE_USB_TRANSPORT_H
#include <libusb.h>
#include <stdint.h>
#include <stddef.h>
#include <signal.h>
extern volatile sig_atomic_t usb_cancelled;
struct usb_link {
    libusb_context *ctx;
    libusb_device_handle *dev;
    uint8_t ports[8], port_count, bus;
    int trace;
    unsigned revision;
    int lines_active;
    int claimed;
    void *network;
};
int usb_open(struct usb_link *l, const char *location, int describe);
int usb_reset_link(const char *location, int trace);
void usb_close(struct usb_link *l);
int usb_lines(struct usb_link *l, int value);
int usb_write(struct usb_link *l, const void *p, size_t n, unsigned timeout);
int usb_read(struct usb_link *l, void *p, size_t n, unsigned timeout);
#endif
