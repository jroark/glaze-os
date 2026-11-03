#ifndef GLAZE_BT_CONTROLLER_H
#define GLAZE_BT_CONTROLLER_H
#include "bcsp.h"
#include "serial.h"
#include <glaze/bluetooth.h>
struct bt_controller {
    struct bcsp link;
    struct bt_serial serial;
    unsigned acl_mtu, acl_credit, acl_total;
    int (*write)(void *,const uint8_t *,unsigned);
    void *write_arg;
    struct bt_status status;
    unsigned phase, credit, opcode, name_index, name_mode;
    uint32_t now, command_deadline, operation_deadline, total_deadline;
    bool cancel, inquiry, logged;
};
void bt_controller_init(struct bt_controller *, int (*)(void *,const uint8_t *,unsigned), void *, uint32_t, uint32_t);
void bt_controller_tick(struct bt_controller *, uint32_t);
void bt_controller_feed(struct bt_controller *,const uint8_t *,unsigned,uint32_t);
void bt_controller_event(void *,const uint8_t *,unsigned);
int bt_controller_scan(struct bt_controller *);
int bt_controller_cancel(struct bt_controller *);
#endif
