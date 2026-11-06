/* Wire facts: hansendc/eyefi-config, eyefi-config.h's request structures
 * and eyefi-config.c's card_info_cmd/wait_for_response. Independent
 * implementation. eyefi_query() stays limited to non-secret status; the
 * eyefi_req_* encoders serve the research commands in research.c. */
#include "protocol.h"
#include <glaze/abi.h>
#include <string.h>

static uint32_t sequence(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1]<<8 |
           (uint32_t)b[2]<<16 | (uint32_t)b[3]<<24;
}

int eyefi_request(const struct eyefi_io *io, const uint8_t *req, unsigned len,
                  uint8_t *buf)
{
    if (!len || len > EYEFI_REQUEST_MAX) return GZ_EINVAL;
    int r = io->read(io->ctx, EYEFI_RSPC, buf);
    if (r < 0) return r;
    uint32_t seq = sequence(buf) + 1;
    if (!seq) seq = 1;
    memset(buf, 0, EYEFI_SIZE);
    memcpy(buf, req, len);
    if ((r = io->write(io->ctx, EYEFI_REQM, buf)) < 0) return r;
    memset(buf, 0, EYEFI_SIZE);
    for (unsigned i = 0; i < 4; ++i) buf[i] = (uint8_t)(seq >> (8*i));
    if ((r = io->write(io->ctx, EYEFI_REQC, buf)) < 0) return r;
    for (unsigned i = 0; i < 50; ++i) {
        if ((r = io->read(io->ctx, EYEFI_RSPC, buf)) < 0) return r;
        if (sequence(buf) == seq) {
            r = io->read(io->ctx, EYEFI_RSPM, buf);
            return r < 0 ? r : 0;
        }
        io->sleep(io->ctx, 300);
    }
    return GZ_ETIMEDOUT;
}

int eyefi_query(const struct eyefi_io *io, unsigned query, uint8_t *buf)
{
    uint8_t req[2];
    if (query != 1 && query != 2 && query != 10 && query != 11 && query != 13)
        return GZ_EINVAL;
    req[0] = 'o'; req[1] = (uint8_t)query;
    int r = eyefi_request(io, req, sizeof(req), buf);
    if (r < 0) return r;
    unsigned length = buf[0];
    if ((query == 1 && length != 6) ||
        ((query == 2 || query == 13) && length > 32) ||
        ((query == 10 || query == 11) && length != 1))
        return GZ_ECORRUPT;
    return (int)length;
}

int eyefi_req_info(uint8_t *req, unsigned token)
{
    if (token > 255) return GZ_EINVAL;
    req[0] = 'o'; req[1] = (uint8_t)token;
    return 2;
}

/* struct card_config_cmd: 'O', subcommand, var_byte_response {len, bytes}. */
int eyefi_req_config(uint8_t *req, unsigned token, const uint8_t *data, unsigned len)
{
    if (token > 255 || len > 255 || 3 + len > EYEFI_REQUEST_MAX) return GZ_EINVAL;
    req[0] = 'O'; req[1] = (uint8_t)token; req[2] = (uint8_t)len;
    if (len) memcpy(req + 3, data, len);
    return (int)(3 + len);
}

/* wlan_disable(): 'O', WLAN_DISABLE, one byte with no length prefix. */
int eyefi_req_wlan(uint8_t *req, int disable)
{
    req[0] = 'O'; req[1] = 10; req[2] = disable ? 1 : 0;
    return 3;
}

int eyefi_req_noarg(uint8_t *req, char command)
{
    if (command != 'g' && command != 'l' && command != 'b' && command != 'S')
        return GZ_EINVAL;
    req[0] = (uint8_t)command;
    return 1;
}

/* struct fetch_log_cmd: 'm', big-endian offset. */
int eyefi_req_log(uint8_t *req, uint32_t offset)
{
    req[0] = 'm';
    for (unsigned i = 0; i < 4; ++i) req[1 + i] = (uint8_t)(offset >> (24 - 8*i));
    return 5;
}

/* struct net_request: req, essid_len, essid[32], network_key {len, key[32]}. */
int eyefi_req_network(uint8_t *req, char command, const char *ssid,
                      const uint8_t *key, unsigned key_len)
{
    size_t n = strlen(ssid);
    if ((command != 'a' && command != 'd' && command != 't') ||
        !n || n > EYEFI_SSID_MAX ||
        (key_len && key_len != 5 && key_len != 13 && key_len != 32) ||
        (command == 'd' && key_len))
        return GZ_EINVAL;
    memset(req, 0, 2 + EYEFI_SSID_MAX + 1 + EYEFI_KEY_MAX);
    req[0] = (uint8_t)command; req[1] = (uint8_t)n;
    memcpy(req + 2, ssid, n);
    req[2 + EYEFI_SSID_MAX] = (uint8_t)key_len;
    if (key_len) memcpy(req + 3 + EYEFI_SSID_MAX, key, key_len);
    return (int)(2 + EYEFI_SSID_MAX + 1 + EYEFI_KEY_MAX);
}
