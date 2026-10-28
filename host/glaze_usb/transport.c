#include "transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
volatile sig_atomic_t usb_cancelled;

static void dump(struct usb_link *l, const char *dir, const uint8_t *p, int n)
{
    if (!l->trace || !n) return;
    if (n == 1 && p[0] == 0x55) return; /* stock CE wake train */
    fprintf(stderr, "%s %d:", dir, n);
    for (int i = 0; i < n && i < 80; i++) fprintf(stderr, " %02x", p[i]);
    if (n > 80) fprintf(stderr, " ...");
    fputc('\n', stderr);
}
static int usb_open_mode(struct usb_link *l, const char *location, int describe, int claim)
{
    int r = libusb_init(&l->ctx);
    if (r) return r;
    libusb_device **list = NULL, *chosen = NULL;
    ssize_t n = libusb_get_device_list(l->ctx, &list);
    int matches = 0;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) || d.idVendor != 0x07cf || d.idProduct != 0x2002) continue;
        uint8_t ports[8]; int count = libusb_get_port_numbers(list[i], ports, sizeof(ports));
        char path[64]; int off = snprintf(path, sizeof(path), "%u", libusb_get_bus_number(list[i]));
        for (int j = 0; j < count; j++) off += snprintf(path + off, sizeof(path) - (size_t)off, ".%u", ports[j]);
        if (describe) printf("BE-300 07cf:2002 location=%s speed=%d\n", path, libusb_get_device_speed(list[i]));
        if (location && strcmp(location, path)) continue;
        l->revision = d.bcdDevice;
        chosen = list[i]; matches++;
    }
    if (matches != 1) {
        fprintf(stderr, matches ? "Multiple BE-300 devices; select --device BUS.PORT.\n" : "No BE-300 found. Wake it and check the USB cable.\n");
        r = LIBUSB_ERROR_NO_DEVICE;
    } else {
        l->bus = libusb_get_bus_number(chosen);
        int pc = libusb_get_port_numbers(chosen, l->ports, sizeof(l->ports));
        l->port_count = pc < 0 ? 0 : (uint8_t)pc;
        r = libusb_open(chosen, &l->dev);
        if (!r && claim) {
            struct libusb_config_descriptor *cfg = NULL;
            r = libusb_get_active_config_descriptor(chosen, &cfg);
            if (r == LIBUSB_ERROR_NOT_FOUND) {
                r = libusb_get_config_descriptor(chosen, 0, &cfg);
                if (!r) r = libusb_set_configuration(l->dev, cfg->bConfigurationValue);
            }
            int in = 0, out = 0, intr = 0;
            if (!r) {
                for (int i = 0; i < cfg->bNumInterfaces; i++)
                    for (int j = 0; j < cfg->interface[i].num_altsetting; j++) {
                        const struct libusb_interface_descriptor *a = &cfg->interface[i].altsetting[j];
                        if (a->bInterfaceNumber || a->bAlternateSetting) continue;
                        for (int k = 0; k < a->bNumEndpoints; k++) {
                            const struct libusb_endpoint_descriptor *e = &a->endpoint[k];
                            if (describe) printf("  endpoint=%02x type=%u packet=%u\n", e->bEndpointAddress, e->bmAttributes & 3, e->wMaxPacketSize);
                            in |= e->bEndpointAddress == 0x81 && (e->bmAttributes & 3) == 2 && e->wMaxPacketSize == 64;
                            out |= e->bEndpointAddress == 0x02 && (e->bmAttributes & 3) == 2 && e->wMaxPacketSize == 64;
                            intr |= e->bEndpointAddress == 0x83 && (e->bmAttributes & 3) == 3;
                        }
                    }
                if (!in || !out || !intr) r = LIBUSB_ERROR_NOT_SUPPORTED;
            }
            if (cfg) libusb_free_config_descriptor(cfg);
            if (!r) r = libusb_claim_interface(l->dev, 0);
            if (!r) {
                l->claimed = 1;
                uint8_t actual[18];
                int got = libusb_control_transfer(l->dev, 0x80, 6, 0x0100, 0, actual, sizeof(actual), 1000);
                if (got == 18) l->revision = (unsigned)actual[12] | (unsigned)actual[13] << 8;
            }
            if (!r && describe) puts("  interface 0 claimed");
        }
    }
    if (list) libusb_free_device_list(list, 1);
    if (r) { fprintf(stderr, "USB: %s\n", libusb_error_name(r)); usb_close(l); }
    return r;
}
int usb_open(struct usb_link *l, const char *location, int describe)
{
    return usb_open_mode(l, location, describe, 1);
}
int usb_reset_link(const char *location, int trace)
{
    struct usb_link l = { .trace = trace };
    int r = usb_open_mode(&l, location, 0, 0);
    if (!r) {
        r = libusb_reset_device(l.dev);
        /* A changed CE/Glaze descriptor makes libusb discard its old
         * handle after re-enumeration. Close it and rediscover normally. */
        if (r == LIBUSB_ERROR_NOT_FOUND) r = 0;
    }
    if (r) fprintf(stderr, "USB reset: %s\n", libusb_error_name(r));
    usb_close(&l);
    return r;
}
void usb_close(struct usb_link *l)
{
    if (l->dev) {
        if (l->lines_active) libusb_control_transfer(l->dev, 0x21, 0x22, 0, 0, NULL, 0, 1000);
        l->lines_active = 0;
        if (l->claimed) libusb_release_interface(l->dev, 0);
        l->claimed = 0;
        libusb_close(l->dev); l->dev = NULL;
    }
    if (l->ctx) { libusb_exit(l->ctx); l->ctx = NULL; }
}
int usb_lines(struct usb_link *l, int value)
{
    int r = libusb_control_transfer(l->dev, 0x21, 0x22, (uint16_t)value, 0, NULL, 0, 2000);
    if (r >= 0) l->lines_active = value;
    if (l->trace) fprintf(stderr, "control lines=%d result=%d\n", value, r);
    if (l->trace && r >= 0) {
        uint8_t configuration = 0;
        int got = libusb_control_transfer(l->dev, 0x80, 8, 0, 0, &configuration, 1, 1000);
        fprintf(stderr, "device configuration=%u result=%d\n", configuration, got);
    }
    uint8_t state[8]; int done = 0;
    if (r >= 0) {
        for (unsigned i = 0; i < 4; i++) {
            int s = libusb_interrupt_transfer(l->dev, 0x83, state, sizeof(state), &done, 50);
            dump(l, "EP3", state, done);
            if (s) break;
        }
    }
    return r;
}
int usb_write(struct usb_link *l, const void *vp, size_t n, unsigned timeout)
{
    const uint8_t *p = vp;
    while (n) {
        int done = 0, want = n > 16384 ? 16384 : (int)n;
        int r = libusb_bulk_transfer(l->dev, 0x02, (unsigned char *)p, want, &done, timeout);
        dump(l, "H>D", p, done);
        p += done; n -= (size_t)done;
        if (r) {
            if (l->trace) fprintf(stderr, "bulk OUT: %s (%d bytes)\n", libusb_error_name(r), done);
            return r;
        }
        if (!done) return LIBUSB_ERROR_IO;
    }
    return 0;
}
int usb_read(struct usb_link *l, void *p, size_t n, unsigned timeout)
{
    /* wceusbsh continuously consumes EP3 serial/data notifications. Keep
     * that endpoint moving alongside bulk IN, and avoid starving CE's
     * shared RX/TX DMA engine with an unpaced stream of one-byte wake INs. */
    uint8_t state[8]; int notified = 0;
    int notification = libusb_interrupt_transfer(l->dev, 0x83, state, sizeof(state), &notified, 1);
    if (!notification) dump(l, "EP3", state, notified);
    int done = 0;
    int r = libusb_bulk_transfer(l->dev, 0x81, p, (int)n, &done, timeout);
    dump(l, "D>H", p, done);
    if (l->trace && r && r != LIBUSB_ERROR_TIMEOUT) fprintf(stderr, "bulk IN: %s (%d bytes)\n", libusb_error_name(r), done);
    return done ? done : r ? r : 0;
}
