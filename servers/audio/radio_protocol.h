/* Bounded, incremental HTTP/ICY parsing, also compiled by host tests. */
#ifndef RADIO_PROTOCOL_H
#define RADIO_PROTOCOL_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <glaze/radio.h>

#define RADIO_SERVERS 16
struct radio_playlist { int count; char urls[RADIO_SERVERS][RADIO_URL_MAX]; };
int radio_parse_playlist(char *text, struct radio_playlist *p);

typedef int (*radio_body_fn)(void *, const uint8_t *, int);
typedef void (*radio_title_fn)(void *, const char *);
struct radio_http {
    char head[8192], line[96], location[RADIO_URL_MAX], type[96];
    int head_len, line_len, status, chunk_state;
    bool headers, chunked, done;
    int64_t remaining;
    uint32_t chunk_left, metaint, audio_left, meta_left;
    int meta_len;
    char meta[4081];
    radio_body_fn body;
    radio_title_fn title;
    void *ctx;
};
void radio_http_init(struct radio_http *h, radio_body_fn body, radio_title_fn title, void *ctx);
/* 0 pending, 1 response complete, -1 malformed; callbacks may return errors. */
int radio_http_feed(struct radio_http *h, const uint8_t *data, int n);
int radio_http_eof(const struct radio_http *h);
#endif
