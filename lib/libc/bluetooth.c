#include <glaze/bluetooth.h>
#include <glaze/sys.h>
#include <string.h>
#include <stddef.h>
static int call_port(int port,unsigned op,struct gz_msg *m,struct gz_msg *r)
{
    if (port<0) return GZ_ENOENT;
    m->w[0]=op;
    int error=gz_send(port,m,r);
    return error<0 ? error : (int)r->w[0];
}
static int call(unsigned op,struct gz_msg *m,struct gz_msg *r)
{ return call_port(gz_port_lookup(BT_PORT_NAME),op,m,r); }
int bt_status(struct bt_status *out)
{
    if (!out) return GZ_EINVAL;
    memset(out,0,sizeof(*out));
    struct gz_msg m={{0}},r={{0}};
    int port=gz_port_lookup(BT_PORT_NAME);
    int error=call_port(port,BT_STATUS,&m,&r);
    if (error) {
        struct bt_card_info card;
        if (!gz_bt_card_info(&card,sizeof(card)) && card.kind==BT_CARD_BLUETOOTH) out->state=BT_STARTING;
        return error;
    }
    memcpy(out,&r.w[1],offsetof(struct bt_status,devices));
    if (out->count>BT_DEVICE_MAX || out->state>BT_FAILED) return GZ_ECORRUPT;
    for (unsigned i=0; i<out->count; i++) {
        m=(struct gz_msg){{0}}; m.w[1]=out->revision; m.w[2]=i; m.w[3]=out->generation;
        error=call_port(port,BT_DEVICE,&m,&r);
        if (error) { out->count=0; return error; }
        memcpy(&out->devices[i],&r.w[1],sizeof(struct bt_device));
        out->devices[i].name[BT_NAME_MAX-1]=0;
    }
    return 0;
}
int bt_scan(void)
{ struct gz_msg m={{0}},r={{0}}; return call(BT_SCAN,&m,&r); }
int bt_cancel(void)
{ struct gz_msg m={{0}},r={{0}}; return call(BT_CANCEL,&m,&r); }

int bt_serial_connect_channel(const uint8_t peer[6],const void *pin,unsigned length,unsigned channel,struct bt_serial_handle *out)
{
    if(!peer || !pin || !out || !length || length>16 || channel>30)return GZ_EINVAL;
    struct gz_msg m={{0}},r={{0}};uint8_t *bytes=(uint8_t *)&m.w[1];
    memcpy(bytes,peer,6);bytes[6]=(uint8_t)length;memcpy(bytes+7,pin,length);bytes[23]=(uint8_t)channel;
    int port=gz_port_lookup(BT_PORT_NAME),error=call_port(port,BT_SERIAL_CONNECT,&m,&r);
    memset(&m,0,sizeof(m));
    if(!error){out->port=port;out->id=r.w[1];}return error;
}
int bt_serial_connect(const uint8_t peer[6],const void *pin,unsigned n,struct bt_serial_handle *out)
{return bt_serial_connect_channel(peer,pin,n,0,out);}

int bt_serial_status(struct bt_serial_status *out)
{
    if(!out)return GZ_EINVAL;
    struct gz_msg m={{0}},r={{0}};int error=call(BT_SERIAL_STATUS,&m,&r);
    memset(out,0,sizeof(*out));if(!error)memcpy(out,&r.w[1],sizeof(*out));return error;
}
int bt_serial_write(struct bt_serial_handle handle,const void *p,unsigned n)
{
    if(n>48 || (n&&!p))return GZ_EINVAL;
    struct gz_msg m={{0}},r={{0}};m.w[1]=handle.id;m.w[2]=n;if(n)memcpy(&m.w[3],p,n);
    return call_port(handle.port,BT_SERIAL_WRITE,&m,&r);
}
int bt_serial_read(struct bt_serial_handle handle,void *p,unsigned n)
{
    if(n>48 || (n&&!p))return GZ_EINVAL;
    struct gz_msg m={{0}},r={{0}};m.w[1]=handle.id;m.w[2]=n;
    int result=call_port(handle.port,BT_SERIAL_READ,&m,&r);
    if(result>0){if((unsigned)result>n)return GZ_ECORRUPT;memcpy(p,&r.w[1],(unsigned)result);}return result;
}
int bt_serial_disconnect(struct bt_serial_handle handle)
{struct gz_msg m={{0}},r={{0}};m.w[1]=handle.id;return call_port(handle.port,BT_SERIAL_CLOSE,&m,&r);}
int bt_serial_disconnect_current(void)
{
    int port=gz_port_lookup(BT_PORT_NAME);
    struct gz_msg m={{0}},r={{0}};
    int error=call_port(port,BT_SERIAL_STATUS,&m,&r);
    if(error)return error;
    struct bt_serial_status status;memcpy(&status,&r.w[1],sizeof(status));
    if(status.state==BT_SERIAL_IDLE)return 0;
    return bt_serial_disconnect((struct bt_serial_handle){port,status.id});
}

int bt_audio_connect(const uint8_t peer[6],const void *pin,unsigned n,struct bt_serial_handle *out)
{
    if(!peer || !pin || !out || !n || n>16)return GZ_EINVAL;
    struct gz_msg m={{0}},r={{0}};uint8_t *p=(uint8_t *)&m.w[1];
    memcpy(p,peer,6);p[6]=(uint8_t)n;memcpy(p+7,pin,n);
    int port=gz_port_lookup(BT_PORT_NAME),error=call_port(port,BT_AUDIO_CONNECT,&m,&r);
    memset(&m,0,sizeof(m));if(!error){out->port=port;out->id=r.w[1];}return error;
}
int bt_audio_status(struct bt_audio_status *out)
{
    if(!out)return GZ_EINVAL;
    struct gz_msg m={{0}},r={{0}};int error=call(BT_AUDIO_STATUS,&m,&r);
    memset(out,0,sizeof(*out));if(!error)memcpy(out,&r.w[1],sizeof(*out));return error;
}
int bt_audio_disconnect_current(void)
{
    int port=gz_port_lookup(BT_PORT_NAME);struct gz_msg m={{0}},r={{0}};
    int error=call_port(port,BT_AUDIO_STATUS,&m,&r);if(error)return error;
    struct bt_audio_status st;memcpy(&st,&r.w[1],sizeof(st));if(st.state==BT_AUDIO_IDLE)return 0;
    m.w[1]=st.id;return call_port(port,BT_AUDIO_CLOSE,&m,&r);
}
int bt_audio_tone(unsigned ms)
{
    int port=gz_port_lookup(BT_PORT_NAME);struct gz_msg m={{0}},r={{0}};
    int error=call_port(port,BT_AUDIO_STATUS,&m,&r);if(error)return error;
    struct bt_audio_status st;memcpy(&st,&r.w[1],sizeof(st));m.w[1]=st.id;m.w[2]=ms;
    return call_port(port,BT_AUDIO_TONE,&m,&r);
}
static int audio_stream_call(struct bt_audio_stream *s,unsigned op,unsigned n)
{
    struct gz_msg m={{0}},r={{0}};m.w[1]=s->id;m.w[2]=s->token;m.w[3]=n;
    return call_port(s->port,op,&m,&r);
}
int bt_audio_stream_open(struct bt_audio_stream *s)
{
    if(!s)return GZ_EINVAL;
    memset(s,0,sizeof(*s));s->port=gz_port_lookup(BT_PORT_NAME);
    struct gz_msg m={{0}},r={{0}};int error=call_port(s->port,BT_AUDIO_STATUS,&m,&r);if(error)return error;
    struct bt_audio_status st;memcpy(&st,&r.w[1],sizeof(st));if(st.state!=BT_AUDIO_STREAMING)return GZ_ENOTCONN;
    s->id=st.id;m.w[1]=s->id;m.w[2]=(uint32_t)gz_getpid();
    error=call_port(s->port,BT_AUDIO_ATTACH,&m,&r);if(error)return error;
    s->shm=(int)r.w[1];s->token=r.w[2];s->buffer=gz_shm_map(s->shm);
    if(!s->buffer){audio_stream_call(s,BT_AUDIO_DETACH,0);return GZ_ENOMEM;}return 0;
}
int bt_audio_stream_write(struct bt_audio_stream *s,const void *p,unsigned n)
{
    if(!s || !s->buffer || (n&&!p))return GZ_EINVAL;
    n=MIN(n,BT_AUDIO_BUF_SIZE);if(n && p!=s->buffer)memcpy(s->buffer,p,n);
    return audio_stream_call(s,BT_AUDIO_WRITE,n);
}
int bt_audio_stream_play(struct bt_audio_stream *s,bool play)
{return s && s->buffer?audio_stream_call(s,play?BT_AUDIO_PLAY:BT_AUDIO_PAUSE,0):GZ_EINVAL;}
int bt_audio_stream_flush(struct bt_audio_stream *s)
{return s && s->buffer?audio_stream_call(s,BT_AUDIO_FLUSH,0):GZ_EINVAL;}
void bt_audio_stream_close(struct bt_audio_stream *s)
{
    if(!s || !s->buffer)return;
    audio_stream_call(s,BT_AUDIO_DETACH,0);gz_shm_unmap(s->buffer);memset(s,0,sizeof(*s));
}
