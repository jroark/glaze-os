/* A synthetic classic peer drives pairing, L2CAP, SDP and RFCOMM end to end. */
#include "../../servers/bluetooth/serial.c"
#include <assert.h>
#include <stdio.h>
static unsigned command_op,command_length,acl_count;
static uint8_t command_data[32];
static int hci_command(void *arg,unsigned op,const void *p,unsigned n)
{(void)arg;assert(n<=sizeof(command_data));command_op=op;command_length=n;memcpy(command_data,p,n);return 0;}
static int hci_acl(void *arg,const uint8_t *p,unsigned n)
{(void)arg;assert(n>=4 && n==4u+get16(p+2));acl_count++;return 0;}
static void flush(struct bt_serial *s)
{for(unsigned i=0;i<100 && s->qt!=s->qh;i++)bt_serial_tick(s,s->now);assert(s->qt==s->qh);}
static void l2(struct bt_serial *s,unsigned cid,const uint8_t *p,unsigned n)
{
    uint8_t bytes[1040];assert(n<=1024);put16(bytes,s->handle|0x2000);put16(bytes+2,n+4);
    put16(bytes+4,n);put16(bytes+6,cid);memcpy(bytes+8,p,n);bt_serial_acl(s,bytes,n+8);
}
static void signal_in(struct bt_serial *s,unsigned code,const uint8_t *p,unsigned n)
{uint8_t bytes[100];bytes[0]=(uint8_t)code;bytes[1]=1;put16(bytes+2,n);memcpy(bytes+4,p,n);l2(s,1,bytes,n+4);}
static void configured(struct bt_serial *s,unsigned index,unsigned remote)
{
    uint8_t p[12]={0};put16(p,remote);put16(p+2,s->channels[index].local);signal_in(s,3,p,8);
    put16(p,s->channels[index].local);put16(p+2,0);p[4]=1;p[5]=2;put16(p+6,672);signal_in(s,4,p,8);
    put16(p,s->channels[index].local);put16(p+2,0);put16(p+4,0);signal_in(s,5,p,6);flush(s);
}
static void rf_peer(struct bt_serial *s,unsigned dlci,unsigned control,const uint8_t *p,unsigned n,int credit)
{
    uint8_t bytes[160];unsigned at=0;
    bytes[at++]=(uint8_t)(dlci<<2 | (control==0x73?3:1));bytes[at++]=(uint8_t)control;bytes[at++]=(uint8_t)(n<<1|1);
    if(credit>=0)bytes[at++]=(uint8_t)credit;
    if(n)memcpy(bytes+at,p,n);
    at+=n;bytes[at]=fcs(bytes,(control&0xef)==0xef?2:3);at++;
    l2(s,0x41,bytes,at);
}
static void sdp_part(struct bt_serial *s,const uint8_t *p,unsigned n,bool more)
{
    uint8_t bytes[80]={7,0,s->sdp_id,0,(uint8_t)(n+3+(more?1:0)),0,(uint8_t)n};
    memcpy(bytes+7,p,n);bytes[7+n]=more?1:0;if(more)bytes[8+n]=0xaa;
    l2(s,0x40,bytes,8+n+(more?1:0));flush(s);
}
int main(void)
{
    static const uint8_t services[]={0x35,0x58,0x35,0x2d,0x09,0x00,0x04,0x35,0x0c,0x35,0x03,0x19,0x01,0x00,0x35,0x05,0x19,0x00,0x03,0x08,0x03,0x09,0x01,0x00,0x25,0x17,0x42,0x6c,0x75,0x65,0x74,0x6f,0x6f,0x74,0x68,0x2d,0x49,0x6e,0x63,0x6f,0x6d,0x69,0x6e,0x67,0x2d,0x50,0x6f,0x72,0x74,0x35,0x27,0x09,0x00,0x04,0x35,0x0c,0x35,0x03,0x19,0x01,0x00,0x35,0x05,0x19,0x00,0x03,0x08,0x01,0x09,0x01,0x00,0x25,0x11,0x47,0x6c,0x61,0x7a,0x65,0x20,0x53,0x65,0x72,0x69,0x61,0x6c,0x20,0x45,0x63,0x68,0x6f};
    assert(service_channel(services,sizeof(services))==1);
    const uint8_t uuid32[]={0x35,7,0x1a,0,0,0,3,8,1};assert(find_rfcomm(uuid32,sizeof(uuid32),0)==1);
    const uint8_t sabm[]={3,0x3f,1};assert(fcs(sabm,3)==0x1c); /* standard RFCOMM golden vector */
    struct bt_serial s;bt_serial_init(&s,hci_command,hci_acl,NULL);
    uint8_t peer[]={1,2,3,4,5,6},pin[]={1,2,3,4};s.acl_mtu=27;
    assert(!bt_serial_start(&s,peer,pin,4,19,0));assert(s.acl_mtu==27);
    bt_serial_tick(&s,1);assert(command_op==0x0405 && command_length==13 && !memcmp(command_data,peer,6));
    const uint8_t connected[]={3,11,0,0x42,0,1,2,3,4,5,6,1,0};bt_serial_event(&s,connected,sizeof(connected));
    bt_serial_tick(&s,2);assert(command_op==0x0411);
    const uint8_t pin_request[]={0x16,6,1,2,3,4,5,6};bt_serial_event(&s,pin_request,sizeof(pin_request));
    bt_serial_tick(&s,3);assert(command_op==0x040d && command_length==23 && command_data[6]==4 && !memcmp(command_data+7,pin,4));
    uint8_t key_event[25]={0x18,23,1,2,3,4,5,6};memset(key_event+8,0xa5,16);bt_serial_event(&s,key_event,25);assert(s.key_valid);
    const uint8_t authenticated[]={6,3,0,0x42,0};bt_serial_event(&s,authenticated,sizeof(authenticated));bt_serial_tick(&s,4);assert(command_op==0x0413);
    const uint8_t encrypted[]={8,4,0,0x42,0,1};bt_serial_event(&s,encrypted,sizeof(encrypted));
    assert(s.status.authenticated && s.status.encrypted && s.stage==3);flush(&s);
    configured(&s,0,0x45);assert(s.stage==4);
    const uint8_t attributes[]={0x35,0x13,0x35,0x11,9,0,4,0x35,0x0c,0x35,3,0x19,1,0,0x35,5,0x19,0,3,8,1};
    sdp_part(&s,attributes,7,true);assert(s.stage==4 && s.continuation_length==1);
    sdp_part(&s,attributes+7,sizeof(attributes)-7,false);assert(s.status.channel==1 && s.stage==5);
    configured(&s,1,0x46);assert(s.stage==6);
    rf_peer(&s,0,0x73,NULL,0,-1);flush(&s);assert(s.stage==7);
    const uint8_t pn[]={0x81,0x11,2,0xe0,0,0,127,0,0,7};rf_peer(&s,0,0xef,pn,sizeof(pn),-1);flush(&s);assert(s.stage==8 && s.credit_flow);
    rf_peer(&s,2,0x73,NULL,0,-1);flush(&s);assert(s.stage==9);
    const uint8_t msc[]={0xe1,5,11,0x8d};rf_peer(&s,0,0xef,msc,sizeof(msc),-1);assert(s.status.state==BT_SERIAL_CONNECTED && !s.pin_length);
    bt_serial_event(&s,authenticated,sizeof(authenticated));bt_serial_event(&s,encrypted,sizeof(encrypted));
    assert(s.status.state==BT_SERIAL_CONNECTED && s.stage==10);
    assert(bt_serial_tx(&s,(const uint8_t *)"hello",5)==5);bt_serial_tick(&s,5);flush(&s);assert(s.status.sent==5);
    rf_peer(&s,2,0xff,(const uint8_t *)"hello",5,2);assert(s.status.available==5);
    uint8_t result[6]={0};assert(bt_serial_rx(&s,result,3)==3 && bt_serial_rx(&s,result+3,3)==2 && !strcmp((char *)result,"hello"));
    s.tx_credits=0;
    assert(bt_serial_tx(&s,(const uint8_t *)"next",4)==4);unsigned before=s.tt;
    bt_serial_tick(&s,5);assert(s.tt==before);
    rf_peer(&s,2,0xff,NULL,0,1);bt_serial_tick(&s,5);flush(&s);assert(s.tt==before+4);
    assert(acl_count>8); /* fragmentation exercised with a 27-byte controller MTU */
    struct bt_serial bounded=s;
    bounded.credit_flow=false;bounded.rh=sizeof(bounded.rx)-64;bounded.rt=0;
    uint8_t oversized[127];memset(oversized,0x55,sizeof(oversized));
    rf_peer(&bounded,2,0xef,oversized,sizeof(oversized),-1);assert(bounded.status.state==BT_SERIAL_FAILED && bounded.status.error==GZ_EFULL);
    bounded=s;bounded.th=bounded.tt=0;
    uint8_t output[600]={0};assert(bt_serial_tx(&bounded,output,sizeof(output))==512 && bt_serial_tx(&bounded,output,1)==GZ_EBUSY);
    assert(!bt_serial_close(&s));bt_serial_tick(&s,6);assert(command_op==0x0406);
    const uint8_t closed[]={5,4,0,0x42,0,0x13};bt_serial_event(&s,closed,sizeof(closed));assert(s.status.state==BT_SERIAL_IDLE && s.key_valid);
    assert(!bt_serial_start(&s,peer,pin,4,20,7));assert(s.key_valid);
    s.handle=0x42;s.status.state=BT_SERIAL_DISCOVERING;s.stage=4;
    const uint8_t malformed[]={7,0,0,0,3,0xff,0xff,0};l2(&s,0x40,malformed,sizeof(malformed));assert(s.status.state==BT_SERIAL_FAILED);
    s.handle=0xffff;peer[0]=9;assert(!bt_serial_start(&s,peer,pin,4,21,8));assert(!s.key_valid);
    bt_serial_tick(&s,90009);assert(s.status.state==BT_SERIAL_FAILED && s.status.error==GZ_ETIMEDOUT);
    puts("Bluetooth serial: PIN/key handling, authentication/encryption, fragmented ACL, L2CAP setup, continued SDP, RFCOMM credits/echo, disconnect and malformed input PASS");
}
