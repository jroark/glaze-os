/* BCSP framing and link state, independent of the UART and OS. */
#ifndef GLAZE_BCSP_H
#define GLAZE_BCSP_H
#include <stdint.h>
#include <stdbool.h>
#define BCSP_PAYLOAD 1024u
struct bcsp {
    int (*write)(void *, const uint8_t *, unsigned);
    void (*event)(void *, const uint8_t *, unsigned);
    void (*acl)(void *, const uint8_t *, unsigned);
    void *arg;
    unsigned state, txseq, rxseq, retries;
    int error;
    bool pending, ack, escaped, dropping, started;
    uint32_t deadline, link_deadline, began;
    uint8_t input[BCSP_PAYLOAD+6], wire[530], command[258];
    unsigned input_length, wire_length, wire_offset, command_length, command_seq, pending_channel;
};
void bcsp_init(struct bcsp *, int (*)(void *, const uint8_t *, unsigned),
               void (*)(void *, const uint8_t *, unsigned), void *, uint32_t);
void bcsp_feed(struct bcsp *, const uint8_t *, unsigned, uint32_t);
void bcsp_tick(struct bcsp *, uint32_t);
int bcsp_acl(struct bcsp *, const uint8_t *, unsigned, uint32_t);
int bcsp_command(struct bcsp *, const uint8_t *, unsigned, uint32_t);
#endif
