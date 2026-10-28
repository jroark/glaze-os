/* Casio PC Connect codec, derived from PCLADP.dll and PCLASRV.exe. */
#ifndef GLAZE_PCCONNECT_H
#define GLAZE_PCCONNECT_H
#include "transport.h"
#define PC_MAX 65535u
struct pc_link {
    struct usb_link *usb;
    uint8_t seq;
    uint8_t rx[PC_MAX], input[4096];
    unsigned input_pos, input_len;
    uint32_t result, error;
    unsigned request_data_sum, request_header_sum;
};
static inline unsigned pc_be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static inline uint32_t pc_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void pc_put16(uint8_t *p, unsigned v) { p[0] = v >> 8; p[1] = v; }
static inline void pc_put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
void pc_checksums(uint8_t *p, unsigned n);
int pc_valid(const uint8_t *p, unsigned n);
int pc_connect(struct pc_link *p, struct usb_link *u);
int pc_call(struct pc_link *p, unsigned op, const void *data, unsigned n);
int pc_put(struct pc_link *p, const char *local, const char *remote);
int pc_get(struct pc_link *p, const char *remote, const char *local);
int pc_run(struct pc_link *p, const char *remote, const char *args);
int pc_mkdir(struct pc_link *p, const char *path);
int pc_free_space(struct pc_link *p, const char *path, uint64_t *free_bytes);
int pc_path(uint8_t *out, unsigned cap, const char *path);
int pc_clean_nand(struct pc_link *p, int dry_run);
int pc_reboot(struct pc_link *p);
#endif
