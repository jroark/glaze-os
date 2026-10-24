/* Optional audio-server extension. Use existing AUD_HELLO shared memory;
 * no new libc entry points are needed, including on older OS versions. */
#ifndef GLAZE_RADIO_H
#define GLAZE_RADIO_H
#include <stdint.h>

enum { AUD_RADIO_PLAY = 0x50, AUD_RADIO_STATE, AUD_RADIO_STOP };
enum {
    RADIO_STOPPED, RADIO_CONNECTING, RADIO_BUFFERING, RADIO_PLAYING,
    RADIO_RECONNECTING, RADIO_ERROR
};
enum {
    RADIO_OK, RADIO_E_NETWORK, RADIO_E_TIMEOUT, RADIO_E_CERT,
    RADIO_E_DATE, RADIO_E_PLAYLIST, RADIO_E_FORMAT, RADIO_E_CONNECTION,
    RADIO_E_MEMORY
};
#define RADIO_URL_MAX 1024
#define RADIO_ID_MAX 32
struct radio_request {
    char id[RADIO_ID_MAX];
    char name[64];
    char playlist[RADIO_URL_MAX];
};
struct radio_state {
    uint32_t phase, error, bitrate, buffered_bytes, retries;
    char id[RADIO_ID_MAX];
    char name[64];
    char title[192];
};
#endif
