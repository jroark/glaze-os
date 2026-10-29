/* Paired TLS transport for the Glaze development protocol. */
#ifndef GLAZE_DEBUG_NET_H
#define GLAZE_DEBUG_NET_H
#include <stdint.h>
#define GD_PORT 3000
#define GD_CONFIG "debug.network"
#define GD_ENABLED "debug.enabled"
#define GD_PAIR_PORT "debug-pair" /* One 64-byte pairing record; reply status in w0. */
#define GD_CONFIG_BYTES 64u /* 32-byte PSK, 32-byte ratcheted RNG seed */
#define GD_IDENTITY "glaze-debug-v1"
#define GD_CIPHER "ECDHE-PSK-AES128-GCM-SHA256"
#define GD_BOOT_MAGIC 0x444e5a47u
#define GD_BOOT_OFFSET 64u
struct gd_boot {
    uint32_t version, enabled;
    uint8_t config[GD_CONFIG_BYTES], entropy[32];
    uint32_t wifi_size;
    uint8_t wifi[128];
};
#endif
