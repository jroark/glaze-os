/* Synthetic legacy controller and A2DP sink; the emitted SBC can also be
 * decoded by an independent codec (ffmpeg) to check the actual audio. */
#include "../../servers/bluetooth/a2dp.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static FILE *sbc_file;
static uint8_t reassembled[1028];
static unsigned have,want,media_packets,rtp_time,rtp_sequence,sdp_replies;
static unsigned le(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}
static void little(uint8_t *p,unsigned n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static int command(void *arg,unsigned op,const void *p,unsigned n){return 0;}
static int outgoing(void *arg,const uint8_t *p,unsigned n)
{
    assert(n>=4 && le(p+2)==n-4);
    if((p[1]&0x30)==0x20){have=0;want=4+le(p+4);}
    assert(have+n-4<=sizeof(reassembled));memcpy(reassembled+have,p+4,n-4);have+=n-4;
    if(have!=want)return 0;
    if(le(reassembled+2)==0x53){
        const uint8_t expected[]={3,0x12,0x34,0,9,0,1,0,1,0,1,0,1,0};
        assert(le(reassembled)==sizeof(expected) && !memcmp(reassembled+4,expected,sizeof(expected)));sdp_replies++;
    }
    if(le(reassembled+2)==0x52){
        const uint8_t *v=reassembled+4;unsigned count=v[12],size=le(reassembled);
        assert(size>=13 && v[0]==0x80 && v[1]==96 && count && count<=15);
        assert(((unsigned)v[2]<<8|v[3])==(rtp_sequence++&65535));
        assert(((unsigned)v[4]<<24|(unsigned)v[5]<<16|(unsigned)v[6]<<8|v[7])==rtp_time);
        rtp_time+=count*128;unsigned at=13;
        for(unsigned i=0;i<count;i++){
            assert(at+4<=size && v[at]==0x9c);
            unsigned mode=(v[at+1]>>2)&3,channels=mode?2:1,pool=v[at+2];
            unsigned bytes=4+4*channels+2*pool+(mode==3);
            assert(at+bytes<=size);
            if(sbc_file)assert(fwrite(v+at,1,bytes,sbc_file)==bytes);
            at+=bytes;
        }
        assert(at==size);media_packets++;
    }
    return 0;
}
static void drain(struct bt_serial *s){for(unsigned i=0;i<100 && s->qh!=s->qt;i++)bt_serial_tick(s,s->now);assert(s->qh==s->qt);}
static void inbound(struct bt_serial *s,unsigned cid,const uint8_t *p,unsigned n)
{
    uint8_t bytes[1040];assert(n<=1024);little(bytes,s->handle|0x2000);little(bytes+2,n+4);
    little(bytes+4,n);little(bytes+6,cid);memcpy(bytes+8,p,n);bt_serial_acl(s,bytes,n+8);
}
static void signal_in(struct bt_serial *s,unsigned code,const uint8_t *p,unsigned n)
{uint8_t b[64]={(uint8_t)code,1};little(b+2,n);memcpy(b+4,p,n);inbound(s,1,b,n+4);}
static void channel(struct bt_serial *s,unsigned index,unsigned remote)
{
    uint8_t p[8]={0};little(p,remote);little(p+2,s->channels[index].local);signal_in(s,3,p,8);
    little(p,s->channels[index].local);little(p+2,0);p[4]=1;p[5]=2;little(p+6,672);signal_in(s,4,p,8);
    memset(p+2,0,6);signal_in(s,5,p,6);drain(s);
}
static void accept(struct bt_a2dp *a,const uint8_t *p,unsigned n)
{uint8_t b[1024]={(uint8_t)(a->transaction<<4|2),a->pending};assert(n<=1022);if(n)memcpy(b+2,p,n);inbound(a->link,0x41,b,n+2);drain(a->link);}
static void setup(struct bt_a2dp *a,struct bt_serial *s)
{
    uint8_t peer[6]={1,2,3,4,5,6};bt_serial_init(s,command,outgoing,NULL);s->acl_mtu=254;
    bt_a2dp_init(a,s);assert(!bt_a2dp_start(a,peer,(const uint8_t *)"0000",4,1,0));
    assert(bt_serial_start(s,peer,(const uint8_t *)"0000",4,2,0)==GZ_EBUSY);
    const uint8_t connected[]={3,11,0,0x42,0,1,2,3,4,5,6,1,0};bt_serial_event(s,connected,sizeof(connected));
    const uint8_t auth[]={6,3,0,0x42,0},enc[]={8,4,0,0x42,0,1};bt_serial_event(s,auth,sizeof(auth));bt_serial_event(s,enc,sizeof(enc));drain(s);
    channel(s,0,0x50);assert(s->stage==4 && s->service_uuid==0x110b);
    /* Continued SDP AudioSink ProtocolDescriptorList. */
    const uint8_t attrs[]={0x35,0x0f,0x35,0x0d,9,0,4,0x35,8,0x35,6,0x19,1,0,9,0,0x19,0};
    /* Exclude trailing padding byte: outer record is 17 bytes. */
    uint8_t sdp[40]={7,0,s->sdp_id,0,12,0,8};memcpy(sdp+7,attrs,8);sdp[15]=1;sdp[16]=0xaa;inbound(s,0x40,sdp,17);drain(s);
    sdp[2]=s->sdp_id;sdp[4]=12;sdp[6]=9;memcpy(sdp+7,attrs+8,9);sdp[16]=0;inbound(s,0x40,sdp,17);drain(s);
    assert(a->status.state!=BT_AUDIO_FAILED);
    /* A real sink's reverse AudioSource SDP query, on a separate CID. */
    const uint8_t incoming[]={1,0,0x53,0};signal_in(s,2,incoming,sizeof(incoming));drain(s);
    assert(s->channels[3].remote==0x53);
    uint8_t config[8]={0x43,0,0,0,1,2,0xa0,2};signal_in(s,4,config,8);
    memset(config+2,0,6);signal_in(s,5,config,6);drain(s);
    const uint8_t query[]={2,0x12,0x34,0,8,0x35,3,0x19,0x11,0x0a,0,16,0};
    unsigned previous=sdp_replies;inbound(s,0x43,query,sizeof(query));drain(s);assert(sdp_replies==previous+1);
    const uint8_t close_sdp[]={0x43,0,0x53,0};signal_in(s,6,close_sdp,4);drain(s);
    assert(!s->channels[3].remote && s->status.state!=BT_SERIAL_FAILED);
    channel(s,1,0x51);assert(a->pending==DISCOVER);
    const uint8_t endpoints[]={4,0,8,8,12,8};accept(a,endpoints,sizeof(endpoints));assert(a->seid==2 && a->pending==CAPS);
    /* Skip an endpoint lacking mandatory codec support. */
    const uint8_t unsupported[]={1,0,7,2,0,2};accept(a,unsupported,sizeof(unsupported));assert(a->seid==3);
    const uint8_t caps[]={1,0,7,6,0,0,0x3f,0xff,2,53};accept(a,caps,sizeof(caps));assert(a->pending==CONFIGURE);
    accept(a,NULL,0);assert(a->pending==OPEN && a->status.rate==44100 && a->status.channels==2);
    accept(a,NULL,0);assert(a->phase==4);channel(s,2,0x52);assert(a->pending==START);
    accept(a,NULL,0);bt_a2dp_tick(a,0);assert(a->status.state==BT_AUDIO_STREAMING && a->status.authenticated && a->status.encrypted);
}
int main(int argc,char **argv)
{
    static struct bt_serial s;static struct bt_a2dp a;setup(&a,&s);
    if(argc==2){sbc_file=fopen(argv[1],"wb");assert(sbc_file);}
    assert(bt_a2dp_tone(&a,3001)==GZ_EINVAL);assert(!bt_a2dp_tone(&a,1000));
    uint8_t zeros[256]={0};assert(bt_a2dp_write(&a,zeros,sizeof(zeros))==GZ_EBUSY);
    for(unsigned now=10;now<=1500;now+=10){s.now=now;bt_a2dp_tick(&a,now);drain(&s);}
    assert(media_packets>40 && !a.tone_left && a.status.packets==media_packets);
    if(sbc_file){fclose(sbc_file);sbc_file=NULL;}
    /* Whole frames, bounded queue, flush and data-only position clock. */
    assert(bt_a2dp_write(&a,zeros,3)==GZ_EINVAL);
    assert(bt_a2dp_write(&a,zeros,sizeof(zeros))==sizeof(zeros));
    bt_a2dp_tick(&a,1501);assert(a.status.queued==sizeof(zeros) && !a.status.frames_played);a.playing=true;
    for(unsigned now=1510;now<=1600;now+=10){s.now=now;bt_a2dp_tick(&a,now);drain(&s);}
    assert(a.status.frames_played==64 && a.status.queued==0);
    for(unsigned i=0;i<BT_AUDIO_RING_SIZE/sizeof(zeros);i++)assert(bt_a2dp_write(&a,zeros,sizeof(zeros))==sizeof(zeros));
    assert(bt_a2dp_write(&a,zeros,sizeof(zeros))==GZ_EBUSY);bt_a2dp_flush(&a);assert(!a.status.queued);
    /* Timeout, peer rejection, malformed capabilities and fragments. */
    assert(!capabilities(&a,(const uint8_t[]){7,255},2));
    assert(!capabilities(&a,(const uint8_t[]){1,0,7,6,0,0,0x22,0x15,40,53},10));
    a.pending=OPEN;a.deadline=1601;bt_a2dp_tick(&a,1601);assert(a.status.state==BT_AUDIO_FAILED && a.status.error==GZ_ETIMEDOUT);
    s.handle=0xffff;s.status.state=BT_SERIAL_IDLE;setup(&a,&s);
    const uint8_t invalid[]={0x0c,1};received(&a,1,invalid,sizeof(invalid));assert(a.status.state==BT_AUDIO_FAILED);
    s.handle=0xffff;s.status.state=BT_SERIAL_IDLE;setup(&a,&s);
    assert(!bt_serial_close(&s));bt_a2dp_tick(&a,1);assert(a.status.state==BT_AUDIO_CLOSING);
    const uint8_t disconnected[]={5,4,0,0x42,0,0x13};bt_serial_event(&s,disconnected,sizeof(disconnected));bt_a2dp_tick(&a,2);assert(a.status.state==BT_AUDIO_IDLE);
    /* Malformed signaling and capability TLVs from an untrusted peer. */
    uint32_t random=0xabc123;uint8_t fuzz[128];
    for(unsigned trial=0;trial<5000;trial++){
        bt_serial_init(&s,command,outgoing,NULL);bt_a2dp_init(&a,&s);
        s.channels[1].remote=0x51;a.pending=(uint8_t)(1+trial%10);
        for(unsigned i=0;i<sizeof(fuzz);i++){random=random*1664525u+1013904223u;fuzz[i]=(uint8_t)(random>>24);}
        unsigned size=41+trial%(sizeof(fuzz)-40);received(&a,1,fuzz,size);capabilities(&a,fuzz,size);
    }
    puts("Bluetooth audio: encrypted link, continued SDP, endpoint selection, AVDTP, SBC/RTP, pacing, PCM clock/backpressure, timeout and disconnect PASS");
    return 0;
}
