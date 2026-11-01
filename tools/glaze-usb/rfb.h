#ifndef GLAZE_HOST_RFB_H
#define GLAZE_HOST_RFB_H
#include <stdint.h>
struct rfb_source {
    void *context;
    int (*capture)(void *,const uint8_t **pixels);
    int (*input)(void *,const uint8_t *,unsigned bytes);
    int (*keepalive)(void *);
};
/* One already-accepted, nonblocking localhost connection. 0 = disconnected,
 * -1 = invalid/slow viewer, -2 = device failure. No public listener here. */
int rfb_session(int fd,const struct rfb_source *,unsigned fps,int view_only);
#endif
