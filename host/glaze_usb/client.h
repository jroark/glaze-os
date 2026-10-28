#ifndef GLAZE_USB_CLIENT_H
#define GLAZE_USB_CLIENT_H
#include "transport.h"
#include <glaze/usb_wire.h>
#include <stdio.h>
struct gu_client { struct usb_link *usb; uint32_t id, caps; struct gu_parser parser; uint8_t frame[GU_FRAME]; };
int gu_call(struct gu_client *, uint32_t op, const void *data, uint32_t len);
int gu_hello(struct gu_client *, struct usb_link *);
int gu_upload(struct gu_client *, const char *local, const char *remote);
int gu_download(struct gu_client *, const char *remote, const char *local);
int gu_reload(struct gu_client *, const char *directory);
int gu_logs(struct gu_client *, int once);
int gu_screenshot(struct gu_client *, const char *path);
int usb_write_png(FILE *, const uint8_t *pixels);
int gu_script(struct gu_client *, const char *path, const char *screenshot);
int ce_boot(struct usb_link *, const char *directory);
int ce_self_test(struct usb_link *, const char *directory);
#endif
