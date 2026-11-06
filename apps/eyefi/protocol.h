#ifndef EYEFI_PROTOCOL_H
#define EYEFI_PROTOCOL_H
#include <stdint.h>
#define EYEFI_SIZE 16384u
#define EYEFI_REQUEST_MAX 512u
#define EYEFI_SSID_MAX 32u
#define EYEFI_KEY_MAX 32u
enum { EYEFI_REQM, EYEFI_REQC, EYEFI_RSPM, EYEFI_RSPC };
/* Each operation transfers exactly EYEFI_SIZE bytes, synchronously. */
struct eyefi_io {
    void *ctx;
    int (*read)(void *, unsigned, uint8_t *);
    int (*write)(void *, unsigned, const uint8_t *);
    void (*sleep)(void *, unsigned);
};
/* Only non-secret identity/status queries are accepted. Result is a
 * length-prefixed byte string; buf must hold EYEFI_SIZE bytes. */
int eyefi_query(const struct eyefi_io *io, unsigned query, uint8_t *buf);

/* Research interface. Sends len bytes of req (zero-padded to EYEFI_SIZE),
 * commits the next sequence number and leaves the whole matching RSPM in
 * buf. Returns 0, or an error; req must not alias buf. */
int eyefi_request(const struct eyefi_io *io, const uint8_t *req, unsigned len,
                  uint8_t *buf);

/* Request encodings (hansendc/eyefi-config, eyefi-config.h). Each returns
 * the request length, or GZ_EINVAL. req holds EYEFI_REQUEST_MAX bytes. */
int eyefi_req_info(uint8_t *req, unsigned token);                 /* 'o' n */
int eyefi_req_config(uint8_t *req, unsigned token,                 /* 'O' n len data */
                     const uint8_t *data, unsigned len);
int eyefi_req_wlan(uint8_t *req, int disable);                     /* 'O' 10 d */
int eyefi_req_noarg(uint8_t *req, char command);                   /* g l b S */
int eyefi_req_log(uint8_t *req, uint32_t offset);                  /* 'm' be32 */
/* 'a' (add), 'd' (delete) or 't' (test): ssid, then an optional raw key
 * (32-byte WPA PSK, or a 5/13-byte WEP key). */
int eyefi_req_network(uint8_t *req, char command, const char *ssid,
                      const uint8_t *key, unsigned key_len);
#endif
