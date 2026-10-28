#ifndef GLAZE_USB_H
#define GLAZE_USB_H
#include <glaze/abi.h>
#include <glaze/usb_wire.h>
/* SYS_USB_CTL is restricted to the kernel-started USB service. */
enum {
    GZ_USB_READ = 1, GZ_USB_WRITE, GZ_USB_LOG_READ, GZ_USB_STATE,
    GZ_USB_RELOAD_BEGIN, GZ_USB_RELOAD_DATA, GZ_USB_RELOAD_PREPARE,
    GZ_USB_RELOAD_GO, GZ_USB_RELOAD_ABORT, GZ_USB_SCREEN_COPY,
    GZ_USB_SCRIPT_BEGIN, GZ_USB_SCRIPT_DATA, GZ_USB_SCRIPT_START,
    GZ_USB_SCRIPT_STATUS, GZ_USB_SCRIPT_CANCEL, GZ_USB_REBOOT_CE
};
#if defined(__mips__)
#include <glaze/sys.h>
static inline int gz_usb_ctl(uint32_t op, void *buf, uint32_t len)
{ return GZ_SC3(SYS_USB_CTL, op, buf, len); }
#endif
/* Begin manifest, all fields little endian on the wire. */
struct gz_usb_boot {
    uint32_t kernel_bytes, romfs_bytes, ramfs_kb, flags;
    uint32_t kernel_crc, romfs_crc;
};
#endif
