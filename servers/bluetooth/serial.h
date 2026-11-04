/* Shared classic security/L2CAP transport with an outbound SPP profile. */
#ifndef GLAZE_BT_SERIAL_H
#define GLAZE_BT_SERIAL_H
#include <glaze/bluetooth.h>
#define BT_L2_MAX 1024u
struct bt_channel { uint16_t local,remote,mtu; uint8_t configured,peer_configured; };
struct bt_serial {
    struct bt_serial_status status;
    int (*command)(void *,unsigned,const void *,unsigned);
    int (*acl)(void *,const uint8_t *,unsigned);
    void *arg;
    uint32_t now,deadline;
    uint16_t handle,acl_mtu;
    uint8_t peer[6],pin[16],pin_length,key[16],key_peer[6];
    bool key_valid,closing,command_sent,remote_paused,credit_flow;
    /* Optional profile hooks share security, SDP, and basic L2CAP with SPP. */
    void *profile;
    void (*profile_ready)(void *,unsigned);
    void (*profile_data)(void *,unsigned,const uint8_t *,unsigned);
    void (*profile_sdp)(void *,const uint8_t *,unsigned);
    uint16_t service_uuid;
    unsigned stage,security_opcode,security_length;
    uint8_t security[23];
    struct bt_channel channels[4];
    uint8_t signal_id,sdp_id,dlci,tx_credits,rx_credits;
    uint16_t rf_mtu;
    struct { uint16_t length; uint8_t bytes[260]; } queue[8];
    unsigned qh,qt,offset;
    uint8_t reassembly[BT_L2_MAX+4],attributes[BT_L2_MAX],continuation[16];
    unsigned have,want,attribute_length,continuation_length;
    uint8_t rx[1024],tx[512];
    uint32_t rh,rt,th,tt;
};
void bt_serial_init(struct bt_serial *,int (*)(void *,unsigned,const void *,unsigned),int (*)(void *,const uint8_t *,unsigned),void *);
int bt_serial_start(struct bt_serial *,const uint8_t *,const uint8_t *,unsigned,uint32_t,uint32_t);
void bt_serial_tick(struct bt_serial *,uint32_t);
void bt_serial_event(struct bt_serial *,const uint8_t *,unsigned);
void bt_serial_acl(struct bt_serial *,const uint8_t *,unsigned);
bool bt_serial_opcode(unsigned);
int bt_serial_close(struct bt_serial *);
int bt_serial_tx(struct bt_serial *,const uint8_t *,unsigned);
int bt_serial_rx(struct bt_serial *,uint8_t *,unsigned);
void bt_l2_open(struct bt_serial *,unsigned,unsigned);
int bt_l2_send(struct bt_serial *,unsigned,const uint8_t *,unsigned);
void bt_l2_fail(struct bt_serial *,int);
#endif
