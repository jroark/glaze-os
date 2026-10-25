/* The card's 64-byte CF I/O bank and byte-addressed attribute window.
 * Kept separate from protocol parsing so native tests can exercise the driver
 * against the emulator model, including malformed responses and timeouts. */
#ifndef GLAZE_WL54CF_IO_H
#define GLAZE_WL54CF_IO_H
static uint8_t wl_read8(unsigned r) { return REG8(0x0a00c300u + r); }
static uint16_t wl_read16(unsigned r) { return REG16(0x0a00c300u + r); }
static void wl_write8(unsigned r, uint8_t v) { REG8(0x0a00c300u + r) = v; }
static void wl_write16(unsigned r, uint16_t v) { REG16(0x0a00c300u + r) = v; }
/* WinCE's byte-addressed attribute alias (be300 pcmcia_attr.c). D200 is
 * NAND direct I/O, not COR. The alias/bridge remains unverified on hardware. */
static void wl_set_cor(uint8_t v) { REG8(0x0b400100u) = v; }
#endif
