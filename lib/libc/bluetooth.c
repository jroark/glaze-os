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
