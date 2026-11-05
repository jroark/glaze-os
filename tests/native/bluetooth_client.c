/* Current-session controls must bind status and close to one service port. */
#include "settings_sys.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../../lib/libc/bluetooth.c"
int gz_getpid(void){return 1;}
static uint8_t pcm[BT_AUDIO_BUF_SIZE];
void *gz_shm_map(int id){assert(id==3);return pcm;}
int gz_shm_unmap(void *p){return 0;}
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
    } else if(m->w[0]==BT_AUDIO_STATUS){
        struct bt_audio_status st={.id=session,.state=BT_AUDIO_STREAMING,.rate=44100,.channels=2};
        memcpy(&r->w[1],&st,sizeof(st));r->w[0]=(uint32_t)status_error;port=22;
    } else if(m->w[0]>=BT_AUDIO_CLOSE){
        assert(m->w[1]==session);
        if(m->w[0]==BT_AUDIO_ATTACH){assert(m->w[2]==1);r->w[1]=3;r->w[2]=99;}
        else if(m->w[0]==BT_AUDIO_WRITE){assert(m->w[2]==99 && m->w[3]<=sizeof(pcm));r->w[0]=m->w[3];}
        else if(m->w[0]==BT_AUDIO_CLOSE)closed=m->w[1];
        else if(m->w[0]!=BT_AUDIO_TONE)assert(m->w[2]==99);
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
    port=21;status_error=0;closed=0;calls=0;
    assert(!bt_audio_disconnect_current() && closed==session && calls==2);
    port=21;assert(!bt_audio_tone(1000));
    port=21;struct bt_audio_stream stream;assert(!bt_audio_stream_open(&stream));
    assert(stream.port==21 && stream.id==session && stream.token==99 && stream.buffer==pcm);
    uint8_t bytes[BT_AUDIO_BUF_SIZE+4];memset(bytes,0xa5,sizeof(bytes));
    assert(bt_audio_stream_write(&stream,NULL,4)==GZ_EINVAL);
    assert(bt_audio_stream_write(&stream,bytes,sizeof(bytes))==BT_AUDIO_BUF_SIZE && !memcmp(pcm,bytes,sizeof(pcm)));
    assert(!bt_audio_stream_play(&stream,true));assert(!bt_audio_stream_play(&stream,false));assert(!bt_audio_stream_flush(&stream));
    bt_audio_stream_close(&stream);assert(!stream.buffer);bt_audio_stream_close(&stream);
    port=21;status_error=GZ_EDEAD;calls=0;assert(bt_audio_stream_open(&stream)==GZ_EDEAD && calls==1);
    puts("Bluetooth client: current-session disconnect, idle, stale port and errors PASS");return 0;
}
