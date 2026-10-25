#ifndef AUDIO_RADIO_H
#define AUDIO_RADIO_H
#include <glaze/radio.h>
#include <stdbool.h>
#include <stdint.h>

#define RADIO_CAPACITY (128u * 1024u)
#define RADIO_PRIME (24u * 1024u)
int radio_start(const struct radio_request *request);
void radio_stop(void);
void radio_fail(uint32_t error);
void radio_snapshot(struct radio_state *state);
/* Network worker is the producer; the audio loop alone consumes/decodes. */
bool radio_active(void);
uint32_t radio_buffered(uint32_t *epoch);
int radio_read(uint32_t epoch, uint8_t *data, int n);
void radio_output(bool playing, uint32_t bitrate);
#endif
