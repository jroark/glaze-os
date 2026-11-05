#ifndef GLAZE_BT_A2DP_H
#define GLAZE_BT_A2DP_H
#include "serial.h"
#include <sbc_encoder.h>
struct bt_a2dp {
    struct bt_serial *link;
    struct bt_audio_status status;
    SBC_ENC_PARAMS encoder;
    uint8_t endpoints[16],endpoint_count,endpoint_index,seid,transaction,pending,phase;
    uint8_t config[4],fragment[1024],fragment_transaction,fragment_signal,fragment_left;
    unsigned fragment_length;
    uint32_t deadline,last,clock,timestamp,tone_left,tone_phase;
    uint16_t sequence;
    int16_t pcm[256],ring[BT_AUDIO_RING_SIZE/2];
    uint32_t head,tail;
    bool playing;
};
void bt_a2dp_init(struct bt_a2dp *,struct bt_serial *);
int bt_a2dp_start(struct bt_a2dp *,const uint8_t *,const uint8_t *,unsigned,uint32_t,uint32_t);
void bt_a2dp_tick(struct bt_a2dp *,uint32_t);
int bt_a2dp_tone(struct bt_a2dp *,unsigned);
int bt_a2dp_write(struct bt_a2dp *,const void *,unsigned);
void bt_a2dp_flush(struct bt_a2dp *);
#endif
