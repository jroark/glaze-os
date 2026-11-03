/* Current-session controls must bind status and close to one service port. */
#include "settings_sys.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../../lib/libc/bluetooth.c"
static int port = 21, calls, status_error, close_error;
static uint32_t session = 7, state = BT_SERIAL_CONNECTED, closed;
int gz_port_lookup(const char *name) { assert(!strcmp(name,BT_PORT_NAME));return port; }
int gz_bt_card_info(struct bt_card_info *out,uint32_t n) { return GZ_ENOENT; }
int gz_send(int target,const struct gz_msg *m,struct gz_msg *r)
{
    calls++;memset(r,0,sizeof(*r));assert(target==21);
    if(m->w[0]==BT_SERIAL_STATUS) {
        struct bt_serial_status status={.state=state,.id=session};
        memcpy(&r->w[1],&status,sizeof(status));r->w[0]=(uint32_t)status_error;
        port=22; /* A restart/replacement appears before the close. */
    } else {
        assert(m->w[0]==BT_SERIAL_CLOSE);closed=m->w[1];r->w[0]=(uint32_t)close_error;
    }
    return 0;
}
int main(void)
{
    assert(!bt_serial_disconnect_current() && closed==7 && calls==2);
    port=21;state=BT_SERIAL_IDLE;closed=0;calls=0;
    assert(!bt_serial_disconnect_current() && !closed && calls==1);
    port=21;state=BT_SERIAL_CONNECTED;status_error=GZ_EDEAD;calls=0;
    assert(bt_serial_disconnect_current()==GZ_EDEAD && calls==1);
    port=21;status_error=0;close_error=GZ_EDEAD;calls=0;
    assert(bt_serial_disconnect_current()==GZ_EDEAD && calls==2);
    port=-1;calls=0;assert(bt_serial_disconnect_current()==GZ_ENOENT && !calls);
    puts("Bluetooth client: current-session disconnect, idle, stale port and errors PASS");return 0;
}
