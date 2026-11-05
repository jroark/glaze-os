/* The reverse SDP client seen on real speakers: fixed wire requests, a
 * golden service-class reply, MTU/byte-count continuation and malformed input. */
#include "../../servers/bluetooth/source_sdp.c"
#include <assert.h>
#include <stdio.h>
static uint8_t reply[256];static unsigned reply_size;
int bt_l2_send(struct bt_serial *s,unsigned index,const uint8_t *p,unsigned n)
{assert(index==3 && n<=s->channels[index].mtu && n<=sizeof(reply));memcpy(reply,p,n);reply_size=n;return 0;}
int main(void)
{
    struct bt_serial s={0};s.channels[3].mtu=48;
    const uint8_t search_req[]={2,0x12,0x34,0,8,0x35,3,0x19,0x11,0x0a,0,16,0};
    const uint8_t search_reply[]={3,0x12,0x34,0,9,0,1,0,1,0,1,0,1,0};
    assert(!bt_source_sdp(&s,3,search_req,sizeof(search_req)));
    assert(reply_size==sizeof(search_reply) && !memcmp(reply,search_reply,sizeof(search_reply)));
    const uint8_t class_req[]={6,0x12,0x34,0,13,0x35,3,0x19,0x11,0x0a,0,32,0x35,3,9,0,1,0};
    const uint8_t class_reply[]={7,0x12,0x34,0,15,0,12,0x35,10,0x35,8,9,0,1,0x35,3,0x19,0x11,0x0a,0};
    assert(!bt_source_sdp(&s,3,class_req,sizeof(class_req)));
    assert(reply_size==sizeof(class_reply) && !memcmp(reply,class_reply,sizeof(class_reply)));
    uint8_t request[22]={6,0x12,0x34,0,15,0x35,3,0x19,0x11,0x0a,0,16,0x35,5,0x0a,0,0,0xff,0xff,0};
    uint8_t blob[128];unsigned total=0,size=20;
    do {
        assert(!bt_source_sdp(&s,3,request,size));assert(reply[0]==7 && big16(reply+3)==reply_size-5);
        unsigned count=(unsigned)reply[5]<<8|reply[6];assert(count<=16 && total+count<=sizeof(blob));
        memcpy(blob+total,reply+7,count);total+=count;
        unsigned continuation=reply[7+count];assert(continuation==0 || continuation==2);
        if(!continuation)break;
        request[19]=2;request[20]=reply[8+count];request[21]=reply[9+count];request[4]=17;size=22;
    } while(total<sizeof(blob));
    assert(total==sizeof(record)+4 && blob[0]==0x35 && blob[1]==total-2 && blob[2]==0x35 && blob[3]==sizeof(record));
    assert(!memcmp(blob+4,record,sizeof(record)));
    request[19]=2;request[20]=0xff;request[21]=0xff;request[4]=17;
    assert(!bt_source_sdp(&s,3,request,22) && reply[0]==1 && reply[6]==5);
    request[4]=15;request[19]=0;request[15]=request[16]=0xff;request[17]=request[18]=0; /* reversed attribute range */
    assert(!bt_source_sdp(&s,3,request,20) && reply[0]==1 && reply[6]==3);
    uint32_t random=0xc123;uint8_t fuzz[128];
    for(unsigned trial=0;trial<10000;trial++){
        for(unsigned i=0;i<sizeof(fuzz);i++){random=random*1664525u+1013904223u;fuzz[i]=(uint8_t)(random>>24);}
        unsigned n=trial%sizeof(fuzz);if(n>=5){fuzz[3]=0;fuzz[4]=(uint8_t)(n-5);}bt_source_sdp(&s,3,fuzz,n);
    }
    puts("Bluetooth source SDP: AudioSource search, filtered attributes, golden records, continuation/MTU and malformed input PASS");
    return 0;
}
