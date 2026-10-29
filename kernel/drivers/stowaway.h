/* Portable Stowaway byte decoder, shared with the host protocol tests. */
#ifndef GLAZE_STOWAWAY_H
#define GLAZE_STOWAWAY_H
#include <glaze/abi.h>
struct stowaway_decoder {
    bool down[128], caps;
    uint16_t held[96]; /* translated key-down code, including edit shortcuts */
    int repeat;
    uint32_t deadline;
};
typedef void (*stowaway_emit)(uint16_t type, uint16_t code);
bool stowaway_ack(unsigned *state, uint8_t byte);
void stowaway_decode(struct stowaway_decoder *s, uint8_t byte, uint32_t now, stowaway_emit emit);
void stowaway_repeat(struct stowaway_decoder *s, uint32_t now, stowaway_emit emit);
#endif
