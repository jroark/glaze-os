/* Protocol behavior against golden link frames and a synthetic HCI peer. */
#include "../../servers/bluetooth/controller.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t output[4096];
static unsigned output_length, events, limit=256;
static int writer(void *arg,const uint8_t *p,unsigned n)
{
    (void)arg;
    if (n>limit) n=limit;
    assert(output_length+n<=sizeof(output));
    memcpy(output+output_length,p,n); output_length+=n; return (int)n;
}
static void event(void *arg,const uint8_t *p,unsigned n)
{ (void)arg; assert(n==2 && p[0]==1 && p[1]==0); events++; }
static void flush(struct bcsp *b,uint32_t now)
{ for (unsigned i=0; i<100 && b->wire_length; i++) bcsp_tick(b,now); }
static void active(struct bcsp *b)
{
    static const uint8_t sync[]={0xc0,0,0x41,0,0xbe,0xac,0xaf,0xef,0xee,0xc0};
    static const uint8_t conf[]={0xc0,0,0x41,0,0xbe,0xde,0xad,0xd0,0xd0,0xc0};
    bcsp_tick(b,0); flush(b,0);
    assert(output_length==10 && output[5]==0xda);
    output_length=0;
    for (unsigned i=0; i<sizeof(sync); i++) bcsp_feed(b,sync+i,1,1);
    assert(b->state==1);
    bcsp_tick(b,1); flush(b,1); assert(output[5]==0xad); output_length=0;
    bcsp_feed(b,conf,sizeof(conf),2); assert(b->state==2);
}
/* Unreliable ACK, valid header sum, sequence ACK expected next command. */
static void ack(struct bcsp *b,unsigned seq,uint32_t now)
{
    uint8_t p[]={0xc0,(uint8_t)(seq<<3),0,0,(uint8_t)~(seq<<3),0xc0};
    bcsp_feed(b,p,sizeof(p),now);
}
static unsigned rxseq;
static void controller_event(struct bt_controller *c,const uint8_t *p,unsigned n,uint32_t now)
{
    uint8_t wire[530], raw[262]; unsigned used=0;
    raw[0]=(uint8_t)(0x80 | rxseq | c->link.txseq<<3); rxseq=(rxseq+1)&7;
    raw[1]=(uint8_t)((n<<4)|5); raw[2]=(uint8_t)(n>>4);
    raw[3]=(uint8_t)~(raw[0]+raw[1]+raw[2]); memcpy(raw+4,p,n);
    wire[used++]=0xc0;
    for (unsigned i=0; i<n+4; i++) {
        if (raw[i]==0xc0 || raw[i]==0xdb) { wire[used++]=0xdb; wire[used++]=raw[i]==0xc0?0xdc:0xdd; }
        else wire[used++]=raw[i];
    }
    wire[used++]=0xc0; bt_controller_feed(c,wire,used,now);
    output_length=0; bcsp_tick(&c->link,now); flush(&c->link,now); output_length=0;
}
static void completed(struct bt_controller *c,unsigned op,const uint8_t *data,unsigned n,uint32_t now)
{
    uint8_t event_data[257]={0x0e,(uint8_t)(4+n),1,(uint8_t)op,(uint8_t)(op>>8),0};
    memcpy(event_data+6,data,n); controller_event(c,event_data,6+n,now);
}
static void controller_tests(void)
{
    struct bt_controller c; output_length=0; rxseq=0;
    bt_controller_init(&c,writer,NULL,7,0); active(&c.link);
    bt_controller_tick(&c,3); assert(c.opcode==0x0c03); flush(&c.link,3);
    completed(&c,0x0c03,NULL,0,4);
    bt_controller_tick(&c,5); assert(c.opcode==0x1001); flush(&c.link,5);
    const uint8_t version[]={1,2,0,1,10,0,3,0}; completed(&c,0x1001,version,8,6);
    bt_controller_tick(&c,7); assert(c.opcode==0x1003); flush(&c.link,7);
    const uint8_t features[8]={1,2,3,4,5,6,7,8}; completed(&c,0x1003,features,8,8);
    bt_controller_tick(&c,9); assert(c.opcode==0x1009); flush(&c.link,9);
    const uint8_t address[]={1,2,3,4,5,6}; completed(&c,0x1009,address,6,10);
    bt_controller_tick(&c,10); assert(c.opcode==0x1005);flush(&c.link,10);
    const uint8_t buffers[]={0x80,0,0,4,0,0,0};completed(&c,0x1005,buffers,7,10);
    assert(c.acl_credit==4 && c.acl_mtu==128);
    assert(c.status.state==BT_READY && !memcmp(c.status.address,address,6));
    assert(c.status.manufacturer==10 && c.status.generation==7);
    assert(!bt_controller_scan(&c) && bt_controller_scan(&c)==GZ_EBUSY);
    bt_controller_tick(&c,11); assert(c.opcode==0x0401); flush(&c.link,11);
    const uint8_t started[]={0x0f,4,0,1,1,4}; controller_event(&c,started,sizeof(started),12);
    assert(c.inquiry && !c.opcode);
    const uint8_t found[]={2,15,1,11,22,33,44,55,66,1,0,0,4,5,6,0x34,0x12};
    controller_event(&c,found,sizeof(found),13); controller_event(&c,found,sizeof(found),14);
    assert(c.status.count==1 && c.status.devices[0].clock_offset==0x1234);
    const uint8_t done[]={1,1,0}; controller_event(&c,done,sizeof(done),15);
    bt_controller_tick(&c,16); assert(c.opcode==0x0419); flush(&c.link,16);
    const uint8_t name_started[]={0x0f,4,0,1,0x19,4}; controller_event(&c,name_started,sizeof(name_started),17);
    const uint8_t name[]={7,11,0,11,22,33,44,55,66,'T','e','s','t'};
    controller_event(&c,name,sizeof(name),18); bt_controller_tick(&c,19);
    assert(c.status.state==BT_READY && !strcmp(c.status.devices[0].name,"Test"));
    /* Cancel a running inquiry through the controller, retaining results. */
    flush(&c.link,19); output_length=0;
    assert(!bt_controller_scan(&c)); bt_controller_tick(&c,20); flush(&c.link,20);
    controller_event(&c,started,sizeof(started),21);
    assert(!bt_controller_cancel(&c)); bt_controller_tick(&c,22);
    assert(c.opcode==0x0402); flush(&c.link,22); completed(&c,0x0402,NULL,0,23);
    assert(c.status.state==BT_READY);
    /* Missing command completion fails explicitly. */
    output_length=0; flush(&c.link,23); output_length=0;
    assert(!bt_controller_scan(&c)); bt_controller_tick(&c,24); flush(&c.link,24);
    ack(&c.link,c.link.txseq,25); bt_controller_tick(&c,3025);
    assert(c.status.state==BT_FAILED && c.status.error==GZ_ETIMEDOUT);
}
static void recorded_startup_test(void)
{
    struct bt_controller c; output_length=0;
    bt_controller_init(&c,writer,NULL,8,0); active(&c.link);
    /* Exact UART frames from the SMART card: channel 5 carries events. */
    const uint8_t startup[]={0xc0,0x80,0x65,0,0x1a,0x0f,4,0,1,0,0,0xc0};
    bt_controller_feed(&c,startup,sizeof(startup),3);
    bt_controller_tick(&c,4); flush(&c.link,4); bt_controller_tick(&c,5); flush(&c.link,5);
    assert(c.opcode==0x0c03);
    const uint8_t reply[]={0xc0,0x89,0x65,0,0x11,0x0e,4,1,3,0x0c,0,0xc0};
    bt_controller_feed(&c,reply,sizeof(reply),6);
    assert(c.phase==1 && !c.opcode);
}
static void edge_tests(void)
{
    struct bt_controller c; output_length=0; rxseq=0;
    bt_controller_init(&c,writer,NULL,9,0); active(&c.link);
    c.status.state=BT_READY; c.phase=4;
    assert(!bt_controller_scan(&c));
    uint8_t found[]={2,15,1,11,22,33,44,55,66,1,0,0,4,5,6,0x34,0x12};
    for(unsigned i=0;i<20;i++) { found[3]=(uint8_t)i; controller_event(&c,found,sizeof(found),3+i); }
    assert(c.status.count==BT_DEVICE_MAX);
    c.status.count=1; c.phase=5;
    bt_controller_tick(&c,30); assert(c.opcode==0x0419); flush(&c.link,30);
    const uint8_t started[]={0x0f,4,0,1,0x19,4}; controller_event(&c,started,sizeof(started),31);
    bt_controller_tick(&c,2031); assert(c.opcode==0x041a); flush(&c.link,2031);
    completed(&c,0x041a,NULL,0,2032); bt_controller_tick(&c,2033);
    assert(c.status.state==BT_READY && !c.status.devices[0].name[0] && c.status.count==1);
    output_length=0; assert(!bt_controller_scan(&c));
    bt_controller_tick(&c,2040); flush(&c.link,2040);
    const uint8_t inquiry_started[]={0x0f,4,0,1,1,4};
    controller_event(&c,inquiry_started,sizeof(inquiry_started),2041);
    assert(!bt_controller_cancel(&c)); bt_controller_tick(&c,2042); flush(&c.link,2042);
    assert(c.opcode==0x0402);
    const uint8_t inquiry_done[]={1,1,0}; controller_event(&c,inquiry_done,sizeof(inquiry_done),2043);
    const uint8_t cancel_race[]={0x0e,4,1,2,4,0x0c}; controller_event(&c,cancel_race,sizeof(cancel_race),2044);
    assert(c.status.state==BT_READY && !c.status.error);
    output_length=0; rxseq=0; bt_controller_init(&c,writer,NULL,10,0); active(&c.link);
    c.credit=0; bt_controller_tick(&c,20001);
    assert(c.status.state==BT_FAILED && c.status.error==GZ_ETIMEDOUT);
}

int main(void)
{
    struct bcsp b; bcsp_init(&b,writer,event,NULL,0); active(&b);
    const uint8_t command[]={3,0x0c,0}; assert(!bcsp_command(&b,command,sizeof(command),3));
    limit=1; flush(&b,3); limit=256;
    unsigned first=output_length; assert(first && b.pending);
    bcsp_tick(&b,254); flush(&b,254); assert(output_length==2*first);
    ack(&b,1,255); assert(!b.pending); output_length=0;
    /* Golden reliable event without optional CRC. Duplicate must not deliver twice. */
    const uint8_t evt[]={0xc0,0x88,0x25,0,0x52,1,0,0xc0};
    bcsp_feed(&b,evt,sizeof(evt),256); bcsp_feed(&b,evt,sizeof(evt),257); assert(events==1 && b.rxseq==1);
    /* Golden CRC from binascii.crc_hqx on bit-reversed input octets. */
    uint8_t crc_event[]={0xc0,0xc9,0x25,0,0x11,1,0,0x24,0x55,0xc0};
    crc_event[8]^=1; bcsp_feed(&b,crc_event,sizeof(crc_event),257); assert(events==1);
    crc_event[8]^=1; bcsp_feed(&b,crc_event,sizeof(crc_event),257); assert(events==2 && b.rxseq==2);
    /* Bad header, malformed escape and oversized frame recover at delimiter. */
    const uint8_t bad[]={0xc0,0,0,0,0,0xc0,0xdb,0,0xc0}; bcsp_feed(&b,bad,sizeof(bad),258);
    uint8_t noise[1100]; memset(noise,0x55,sizeof(noise)); bcsp_feed(&b,noise,sizeof(noise),259);
    bcsp_feed(&b,evt,sizeof(evt),260); assert(events==2 && !b.error);
    /* A controller reset after establishment invalidates the link. */
    const uint8_t reset[]={0xc0,0,0x41,0,0xbe,0xda,0xdc,0xed,0xed,0xc0};
    bcsp_feed(&b,reset,sizeof(reset),261); assert(b.error==GZ_ECONNRESET);
    output_length=0; bcsp_init(&b,writer,event,NULL,0); active(&b);
    for (unsigned i=0;i<10;i++) {
        assert(!bcsp_command(&b,command,sizeof(command),3+i*10)); flush(&b,3+i*10);
        ack(&b,b.txseq,4+i*10); assert(!b.pending); output_length=0;
    }
    assert(b.txseq==2);
    controller_tests(); recorded_startup_test(); edge_tests();
    puts("bluetooth: BCSP negotiation, partial writes, retransmit, ACK/duplicate, malformed input, HCI init, inquiry, names, cancellation and timeout PASS");
}
