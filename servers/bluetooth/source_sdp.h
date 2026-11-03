#ifndef GLAZE_BT_SOURCE_SDP_H
#define GLAZE_BT_SOURCE_SDP_H
#include "serial.h"
/* AudioSource SDP record, service search and filtered attribute requests. */
int bt_source_sdp(struct bt_serial *,unsigned,const uint8_t *,unsigned);
#endif
