#ifndef GLAZE_USB_CLIENT_H
#define GLAZE_USB_CLIENT_H
#include "transport.h"
#include <glaze/usb_wire.h>
#include <stdbool.h>
#include <stdio.h>
struct gu_client { struct usb_link *usb; uint32_t id, caps; struct gu_parser parser; uint8_t frame[GU_FRAME]; };
int gu_call(struct gu_client *, uint32_t op, const void *data, uint32_t len);
int gu_hello(struct gu_client *, struct usb_link *);
int gu_upload(struct gu_client *, const char *local, const char *remote);
int gu_nand_backup(struct gu_client *, const char *path);
int gu_nand_apply(struct gu_client *, const char *source, const char *target, const char *plan, bool keep_fs);
int gu_cf_info(struct gu_client *, uint32_t *sectors, uint32_t *generation);
int gu_cf_read(struct gu_client *, uint32_t lba, uint32_t count, const char *path);
int gu_download(struct gu_client *, const char *remote, const char *local);
int gu_reload(struct gu_client *, const char *directory);
int gu_logs(struct gu_client *, int once);
int gu_usb_net(struct gu_client *, const char *interface);
int gu_usb_net_pump(struct gu_client *, int fd, int layer3);
int gu_mirror(struct gu_client *, int argc, char **argv);
int gu_screenshot(struct gu_client *, const char *path);
int usb_write_png(FILE *, const uint8_t *pixels);
int gu_script(struct gu_client *, const char *path, const char *screenshot);
int ce_boot(struct usb_link *, const char *directory, int cf_only);
int ce_self_test(struct usb_link *, const char *directory, int cf_only);
#endif
