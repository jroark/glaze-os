#ifndef GLAZE_USB_SCRIPT_H
#define GLAZE_USB_SCRIPT_H
#include <stdint.h>
/* Compile emulator-style text into stable time-sorted 12-byte wire events. */
int usb_script_compile(const char *text, uint8_t *events, unsigned *count, unsigned *line);
#endif
