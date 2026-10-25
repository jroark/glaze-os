#ifndef RADIO_MP3_H
#define RADIO_MP3_H
#include <mp3dec.h>
#include <stdint.h>
struct radio_mp3 {
    HMP3Decoder decoder;
    uint8_t input[8192];
    int len;
};
int radio_mp3_reset(struct radio_mp3 *d);
void radio_mp3_close(struct radio_mp3 *d);
int radio_mp3_feed(struct radio_mp3 *d, const uint8_t *p, int n);
/* One complete frame, or 0 when more input is needed. Output holds 2304 S16. */
int radio_mp3_frame(struct radio_mp3 *d, int16_t *out, MP3FrameInfo *info);
#endif
