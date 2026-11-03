/* Classic HCI discovery only. Bluetooth Core Vol 4 Part E sections 7.1,
 * 7.4, 7.7: command/status credit, inquiry and remote name events.
 */
#include "controller.h"
#include <string.h>
static unsigned le16(const uint8_t *p) { return p[0]|(unsigned)p[1]<<8; }
static void changed(struct bt_controller *c) { c->status.revision++; }
static void failed(struct bt_controller *c, int error)
{ c->status.state=BT_FAILED; c->status.error=error; changed(c); }
static bool due(uint32_t now,uint32_t end) { return (int32_t)(now-end)>=0; }
static int command(struct bt_controller *c,unsigned opcode,const void *data,unsigned length)
{
    if (c->opcode || !c->credit) return GZ_EBUSY;
    uint8_t p[258]; p[0]=(uint8_t)opcode; p[1]=(uint8_t)(opcode>>8); p[2]=(uint8_t)length;
    if (length) memcpy(p+3,data,length);
    int r=bcsp_command(&c->link,p,3+length,c->now);
    if (!r) { c->opcode=opcode; c->credit--; c->command_deadline=c->now+3000; }
    return r;
}
static int write_uart(void *arg,const uint8_t *bytes,unsigned length)
{
    struct bt_controller *c=arg; return c->write(c->write_arg,bytes,length);
}
static int serial_command(void *arg,unsigned op,const void *p,unsigned n)
{ struct bt_controller *c=arg; return command(c,op,p,n); }
static int serial_acl(void *arg,const uint8_t *p,unsigned n)
{
    struct bt_controller *c=arg;
    if(!c->acl_credit)return GZ_EBUSY;
    int r=bcsp_acl(&c->link,p,n,c->now);if(!r)c->acl_credit--;return r;
}
static void received_acl(void *arg,const uint8_t *p,unsigned n)
{ struct bt_controller *c=arg;bt_serial_acl(&c->serial,p,n); }

void bt_controller_init(struct bt_controller *c,int (*write)(void *,const uint8_t *,unsigned),
                        void *arg,uint32_t generation,uint32_t now)
{
    memset(c,0,sizeof(*c)); c->credit=1; c->now=now;
    c->total_deadline=now+20000;
    c->status.state=BT_STARTING; c->status.generation=generation; c->status.revision=1;
    c->write=write; c->write_arg=arg;
    bcsp_init(&c->link,write_uart,bt_controller_event,c,now);
    c->link.acl=received_acl;
    bt_serial_init(&c->serial,serial_command,serial_acl,c);
}
/* Caller supplies controller as the BCSP callback argument. */
int bt_controller_scan(struct bt_controller *c)
{
    if (c->serial.status.state!=BT_SERIAL_IDLE && (c->serial.status.state!=BT_SERIAL_FAILED || c->serial.handle!=0xffff)) return GZ_EBUSY;
    if (c->status.state==BT_SCANNING) return GZ_EBUSY;
    if (c->status.state!=BT_READY) return GZ_ENOTCONN;
    memset(c->status.devices,0,sizeof(c->status.devices)); c->status.count=0;
    c->status.error=0; c->status.state=BT_SCANNING;
    c->phase=4; c->inquiry=false; c->cancel=false; c->name_mode=0; c->name_index=0;
    c->operation_deadline=c->now+14000; c->total_deadline=c->now+55000; changed(c); return 0;
}
int bt_controller_cancel(struct bt_controller *c)
{
    if (c->status.state!=BT_SCANNING) return 0;
    c->cancel=true; return 0;
}
static void ready(struct bt_controller *c)
{ c->status.state=BT_READY; c->cancel=c->inquiry=false; c->name_mode=0; changed(c); }
static void result(struct bt_controller *c,const uint8_t *p)
{
    unsigned index;
    for (index=0; index<c->status.count; index++)
        if (!memcmp(p,c->status.devices[index].address,6)) break;
    if (index==BT_DEVICE_MAX) return;
    if (index==c->status.count) c->status.count++;
    struct bt_device *d=&c->status.devices[index];
    memcpy(d->address,p,6); d->repetition=p[6]; memcpy(d->device_class,p+9,3);
    d->clock_offset=(uint16_t)le16(p+12); changed(c);
}
void bt_controller_event(void *arg,const uint8_t *p,unsigned n)
{
    struct bt_controller *c=arg;
    if (n<2 || n!=2u+p[1] || c->status.state==BT_FAILED) return;
    if (p[0]==0x10) { failed(c,GZ_EIO); return; } /* controller hardware error */
    if (p[0]==0x0e || p[0]==0x0f) {
        if (n<6) return;
        unsigned opcode=p[0]==0x0e ? le16(p+3) : le16(p+4);
        c->credit=p[0]==0x0e ? p[2] : p[3];
        if (opcode!=c->opcode) return;
        unsigned status=p[0]==0x0e ? p[5] : p[2];
        c->opcode=0;
        if(bt_serial_opcode(opcode)){bt_serial_event(&c->serial,p,n);return;}
        if (status) {
            /* Inquiry may complete between Cancel and its command reply. */
            if (opcode==0x0402 && status==0x0c && !c->inquiry) { ready(c); return; }
            if (opcode==0x0419 || opcode==0x041a) {
                c->name_mode=0; c->name_index++;
                if (c->cancel) ready(c);
                return;
            }
            failed(c,GZ_EIO); return;
        }
        if (opcode==0x0c03) c->phase=1;
        else if (opcode==0x1001) {
            if (n<14) { failed(c,GZ_ECORRUPT); return; }
            c->status.hci_version=p[6]; c->status.hci_revision=(uint16_t)le16(p+7);
            c->status.lmp_version=p[9]; c->status.manufacturer=(uint16_t)le16(p+10);
            c->status.lmp_subversion=(uint16_t)le16(p+12); c->phase=2;
        } else if (opcode==0x1003) {
            if (n<14) { failed(c,GZ_ECORRUPT); return; }
            memcpy(c->status.features,p+6,8); c->phase=3;
        } else if (opcode==0x1009) {
            if (n<12) { failed(c,GZ_ECORRUPT); return; }
            memcpy(c->status.address,p+6,6); c->phase=6;
        } else if(opcode==0x1005) {
            if(n<13 || le16(p+6)<27 || !le16(p+9)){failed(c,GZ_ECORRUPT);return;}
            c->acl_mtu=MIN(le16(p+6),254u); c->acl_total=le16(p+9);c->acl_credit=c->acl_total;
            c->serial.acl_mtu=(uint16_t)c->acl_mtu;c->phase=4;ready(c);
        } else if (opcode==0x0401) c->inquiry=true;
        else if (opcode==0x0402) ready(c);
        else if (opcode==0x041a) {
            c->name_mode=0; c->name_index++;
            if (c->cancel) ready(c);
        }
        changed(c); return;
    }
    if(p[0]==0x13 && n>=3 && n==3u+4u*p[2]) {
        for(unsigned i=0;i<p[2];i++)if(le16(p+3+4*i)==c->serial.handle)
            c->acl_credit=MIN(c->acl_total,c->acl_credit+le16(p+5+4*i));
    }
    if((p[0]==5 && n==6 && le16(p+3)==c->serial.handle) ||
       (p[0]==3 && n==13 && !p[2] && !memcmp(p+5,c->serial.peer,6)))c->acl_credit=c->acl_total;
    bt_serial_event(&c->serial,p,n);
    if (c->status.state!=BT_SCANNING) return;
    if (p[0]==2 && c->phase==4 && n>=3 && n==3u+14u*p[2]) {
        for (unsigned i=0; i<p[2]; i++) result(c,p+3+14*i);
    } else if (p[0]==1 && c->phase==4 && n==3) {
        if (p[2]) { failed(c,GZ_EIO); return; }
        c->inquiry=false; c->phase=5; c->name_mode=0; c->name_index=0;
        if (c->cancel) ready(c);
    } else if (p[0]==7 && n>=9 && c->phase==5 && c->name_mode==1 &&
               c->name_index<c->status.count &&
               !memcmp(p+3,c->status.devices[c->name_index].address,6)) {
        if (!p[2]) {
            struct bt_device *d=&c->status.devices[c->name_index];
            unsigned len=n-9; if (len>=BT_NAME_MAX) len=BT_NAME_MAX-1;
            for (unsigned i=0; i<len; i++) d->name[i]=p[9+i]>=32 && p[9+i]<127 ? (char)p[9+i] : p[9+i] ? '?' : 0;
            d->name[len]=0;
        }
        c->name_mode=0; c->name_index++; changed(c);
        if (c->cancel) ready(c);
    }
}
void bt_controller_feed(struct bt_controller *c,const uint8_t *bytes,unsigned n,uint32_t now)
{ c->now=now; bcsp_feed(&c->link,bytes,n,now); }
void bt_controller_tick(struct bt_controller *c,uint32_t now)
{
    c->now=now; bcsp_tick(&c->link,now);
    if (c->status.state==BT_FAILED) return;
    if (c->link.error) { failed(c,c->link.error); return; }
    if ((c->status.state==BT_STARTING || c->status.state==BT_SCANNING) && due(now,c->total_deadline)) { failed(c,GZ_ETIMEDOUT); return; }
    if (c->opcode && due(now,c->command_deadline)) { failed(c,GZ_ETIMEDOUT); return; }
    if(c->status.state==BT_READY)bt_serial_tick(&c->serial,now);
    if (c->link.state!=2 || c->opcode || c->link.pending || !c->credit) return;
    if (c->status.state==BT_STARTING) {
        static const unsigned ops[]={0x0c03,0x1001,0x1003,0x1009};
        if (c->phase<4) command(c,ops[c->phase],NULL,0);
        else if(c->phase==6)command(c,0x1005,NULL,0);
    } else if (c->status.state==BT_SCANNING && c->phase==4) {
        if (c->cancel) {
            if (c->inquiry) command(c,0x0402,NULL,0); else ready(c);
        } else if (!c->inquiry) {
            static const uint8_t params[]={0x33,0x8b,0x9e,8,0};
            command(c,0x0401,params,sizeof(params));
        } else if (due(now,c->operation_deadline)) failed(c,GZ_ETIMEDOUT);
    } else if (c->status.state==BT_SCANNING && c->phase==5) {
        if (c->name_index>=c->status.count) { ready(c); return; }
        struct bt_device *d=&c->status.devices[c->name_index];
        if (c->name_mode==1 && (c->cancel || due(now,c->operation_deadline))) c->name_mode=2;
        if (c->name_mode==2) {
            if (!command(c,0x041a,d->address,6)) c->name_mode=3;
        } else if (!c->name_mode) {
            if (c->cancel) { ready(c); return; }
            uint8_t params[10]; memcpy(params,d->address,6); params[6]=d->repetition; params[7]=0;
            params[8]=(uint8_t)d->clock_offset; params[9]=(uint8_t)((d->clock_offset>>8)|0x80);
            if (!command(c,0x0419,params,sizeof(params))) { c->name_mode=1; c->operation_deadline=now+2000; }
        }
    }
}
