/* Test-only libusb facade. The native CLI runs unchanged; a Python peer
 * forwards synchronous transfers to the actual emulated USB controller.
 * This file is never linked into the production glaze-usb executable. */
#include <libusb.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
static int fd = -1;
static int transfer(void *p, size_t n, int writing)
{
    uint8_t *b = p;
    while (n) {
        ssize_t k = writing ? write(fd, b, n) : read(fd, b, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        b += k; n -= (size_t)k;
    }
    return 0;
}
static int rpc(uint32_t op, uint32_t a, uint32_t b, uint32_t timeout,
               void *data, int out, int *done)
{
    uint32_t h[4] = {op,a,b,timeout}; int32_t result[2];
    if (transfer(h, sizeof(h), 1) || (out && transfer(data, b, 1)) || transfer(result, sizeof(result), 0)) return LIBUSB_ERROR_IO;
    *done = result[1];
    if (!out && *done && transfer(data, (size_t)*done, 0)) return LIBUSB_ERROR_IO;
    return result[0];
}
int libusb_init(libusb_context **ctx)
{
    const char *path = getenv("GLAZE_USB_TEST_SOCKET");
    if (!path) return LIBUSB_ERROR_INVALID_PARAM;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(a.sun_path)) return LIBUSB_ERROR_INVALID_PARAM;
    strcpy(a.sun_path, path); fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a))) return LIBUSB_ERROR_IO;
    *ctx = (libusb_context *)(uintptr_t)1; return 0;
}
void libusb_exit(libusb_context *ctx) { (void)ctx; if (fd >= 0) close(fd); fd = -1; }
ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list)
{
    (void)ctx; static libusb_device *devices[2] = {(libusb_device *)(uintptr_t)1,NULL};
    *list = devices; return 1;
}
void libusb_free_device_list(libusb_device **list, int unref) { (void)list; (void)unref; }
int libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *d)
{
    (void)dev; uint8_t b[18]; int n = 0;
    int r = rpc(1, 0x80 | 6u << 8 | 0x100u << 16, 18u << 16, 1000, b, 0, &n);
    if (r || n != 18) return LIBUSB_ERROR_NO_DEVICE;
    memcpy(d, b, 18); return 0;
}
uint8_t libusb_get_bus_number(libusb_device *dev) { (void)dev; return 1; }
int libusb_get_port_numbers(libusb_device *dev, uint8_t *p, int n) { (void)dev; if (!n) return -1; p[0] = 1; return 1; }
int libusb_get_device_speed(libusb_device *dev) { (void)dev; return LIBUSB_SPEED_FULL; }
int libusb_open(libusb_device *dev, libusb_device_handle **h)
{
    (void)dev; *h = (libusb_device_handle *)(uintptr_t)1;
    int done; return rpc(3, 1, 0, 1000, NULL, 0, &done);
}
void libusb_close(libusb_device_handle *h) { (void)h; }
int libusb_claim_interface(libusb_device_handle *h, int n) { (void)h; (void)n; return 0; }
int libusb_release_interface(libusb_device_handle *h, int n) { (void)h; (void)n; return 0; }
int libusb_reset_device(libusb_device_handle *h) { (void)h; int n; return rpc(4, 0, 0, 1000, NULL, 0, &n); }
int libusb_set_configuration(libusb_device_handle *h, int n) { (void)h; int done; return rpc(3, (uint32_t)n, 0, 1000, NULL, 0, &done); }
int libusb_get_active_config_descriptor(libusb_device *d, struct libusb_config_descriptor **cfg)
{
    (void)d;
    static const struct libusb_endpoint_descriptor ep[] = {
        {.bEndpointAddress=0x81,.bmAttributes=2,.wMaxPacketSize=64},
        {.bEndpointAddress=2,.bmAttributes=2,.wMaxPacketSize=64},
        {.bEndpointAddress=0x83,.bmAttributes=3,.wMaxPacketSize=8}
    };
    static const struct libusb_interface_descriptor alt = {.bNumEndpoints=3,.endpoint=ep};
    static const struct libusb_interface iface = {.num_altsetting=1,.altsetting=&alt};
    static struct libusb_config_descriptor c = {.bConfigurationValue=1,.bNumInterfaces=1,.interface=&iface};
    *cfg = &c; return 0;
}
int libusb_get_config_descriptor(libusb_device *d, uint8_t n, struct libusb_config_descriptor **c)
{ (void)n; return libusb_get_active_config_descriptor(d, c); }
void libusb_free_config_descriptor(struct libusb_config_descriptor *c) { (void)c; }
int libusb_control_transfer(libusb_device_handle *h, uint8_t rt, uint8_t req, uint16_t value,
                            uint16_t index, unsigned char *b, uint16_t len, unsigned timeout)
{
    (void)h; int n = 0;
    int r = rpc(1, rt | (uint32_t)req << 8 | (uint32_t)value << 16,
                index | (uint32_t)len << 16, timeout, b, !(rt & 0x80), &n);
    return r ? r : n;
}
int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *b, int len,
                         int *done, unsigned timeout)
{ (void)h; return rpc(2, ep, (uint32_t)len, timeout, b, !(ep & 0x80), done); }
int libusb_interrupt_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *b, int len,
                              int *done, unsigned timeout)
{ (void)h; (void)ep; (void)b; (void)len; (void)timeout; *done = 0; return LIBUSB_ERROR_TIMEOUT; }
const char *libusb_error_name(int code)
{
    switch (code) {
    case 0: return "SUCCESS";
    case LIBUSB_ERROR_TIMEOUT: return "LIBUSB_ERROR_TIMEOUT";
    case LIBUSB_ERROR_IO: return "LIBUSB_ERROR_IO";
    case LIBUSB_ERROR_NO_DEVICE: return "LIBUSB_ERROR_NO_DEVICE";
    default: return "LIBUSB_TEST_ERROR";
    }
}
