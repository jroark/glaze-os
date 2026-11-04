/* A2DP source, AVDTP 1.0 basic transport, SBC and RTP (RFC 3550).
 * One local source SEP (1), one explicitly selected remote audio sink.
 * All packet storage, endpoint traversal, transactions and queues are bounded.
 */
#include "a2dp.h"
#include "source_sdp.h"
#include <string.h>
#include <stdio.h>
enum { DISCOVER=1, CAPS, CONFIGURE, GET_CONFIG, RECONFIGURE, OPEN, START, CLOSE, SUSPEND, ABORT };
static bool due(uint32_t now,uint32_t end){return (int32_t)(now-end)>=0;}
static void failed(struct bt_a2dp *a,int error)
{
#ifdef BT_SERIAL_DEBUG
    printf("bluetooth audio: fail pending %u queue %u offset %u\n",a->pending,a->link->qh-a->link->qt,a->link->offset);
#endif
    a->status.state=BT_AUDIO_FAILED;a->status.error=error;a->pending=0;
    a->head=a->tail=0;a->status.queued=0;a->tone_left=0;
    bt_l2_fail(a->link,error);
}
static void request(struct bt_a2dp *a,unsigned signal,const uint8_t *p,unsigned n)
{
    uint8_t bytes[32];
    if(n>sizeof(bytes)-2 || a->pending){failed(a,GZ_ECORRUPT);return;}
    a->transaction=(uint8_t)((a->transaction+1)&15);
    bytes[0]=(uint8_t)(a->transaction<<4);bytes[1]=(uint8_t)signal;
    if(n)memcpy(bytes+2,p,n);
    int error=bt_l2_send(a->link,1,bytes,n+2);
    if(error){failed(a,error);return;}
#ifdef BT_SERIAL_DEBUG
    printf("bluetooth audio: TX signal %u cid %x depth %u\n",signal,a->link->channels[1].remote,a->link->qh-a->link->qt);
#endif
    a->pending=(uint8_t)signal;a->deadline=a->link->now+5000;
}
/* SDP UUID16 L2CAP with uint16 PSM 0x0019. Validate every element even
 * after a match, so truncated or oversized peer records cannot be accepted. */
static int sink_service(const uint8_t *p,unsigned n,unsigned depth)
{
    if(depth>8)return GZ_ECORRUPT;
    bool found=false;
    while(n){
        unsigned type=p[0]>>3,index=p[0]&7,head=1,len=0;
        if(index<5)len=1u<<index;
        else {unsigned bytes=1u<<(index-5);if(n<1+bytes)return GZ_ECORRUPT;
            for(unsigned i=0;i<bytes;i++){if(len>1024)return GZ_ECORRUPT;len=len<<8|p[1+i];}head+=bytes;}
        if(!type)len=0;
        if(head>n || len>n-head)return GZ_ECORRUPT;
        if(type==6){
            const uint8_t descriptor[]={0x19,1,0,9,0,0x19};
            if(len==sizeof(descriptor) && !memcmp(p+head,descriptor,len))found=true;
            int child=sink_service(p+head,len,depth+1);if(child<0)return child;if(child)found=true;
        }
        p+=head+len;n-=head+len;
    }
    return found?1:0;
}
static void services(void *arg,const uint8_t *p,unsigned n)
{
    struct bt_a2dp *a=arg;int found=sink_service(p,n,0);
    if(found<=0){failed(a,found?found:GZ_ENOENT);return;}
    a->link->stage=5;bt_l2_open(a->link,1,0x19);
}
static void ready(void *arg,unsigned index)
{
    struct bt_a2dp *a=arg;
    if(index==1 && !a->phase){a->phase=1;request(a,DISCOVER,NULL,0);}
    else if(index==2 && a->phase==4){uint8_t seid=(uint8_t)(a->seid<<2);a->phase=5;request(a,START,&seid,1);}
}
/* A2DP 1.0 section 4.3.2 SBC capability masks. Prefer stereo, 44.1 kHz,
 * 16 blocks, 8 subbands, loudness and a conservative UART-friendly bitpool. */
static bool capabilities(struct bt_a2dp *a,const uint8_t *p,unsigned n)
{
    bool transport=false,codec=false;uint8_t config[4]={0};
    while(n){
        if(n<2 || p[1]>n-2)return false;
        unsigned cat=p[0],len=p[1];const uint8_t *v=p+2;
        if(cat==1){if(len)return false;transport=true;}
        if(cat==7 && len==6 && !v[0] && !v[1]){
            unsigned freq=(v[2]&0x20)?0x20:(v[2]&0x10)?0x10:0;
            unsigned mode=(v[2]&2)?2:(v[2]&1)?1:(v[2]&8)?8:0;
            unsigned allocation=(v[3]&1)?1:(v[3]&2)?2:0;
            unsigned pool=MAX(6u,(unsigned)v[4]);
            if(!freq || !mode || !(v[3]&0x10) || !(v[3]&4) || !allocation || v[4]<2 || v[5]<pool || pool>32)return false;
            config[0]=(uint8_t)(freq|mode);config[1]=(uint8_t)(0x14|allocation);config[2]=config[3]=(uint8_t)pool;codec=true;
        }
        p+=len+2;n-=len+2;
    }
    if(!transport || !codec)return false;
    memcpy(a->config,config,4);return true;
}
static void next_endpoint(struct bt_a2dp *a)
{
    if(a->endpoint_index>=a->endpoint_count){failed(a,GZ_ENOENT);return;}
    a->seid=a->endpoints[a->endpoint_index++];uint8_t seid=(uint8_t)(a->seid<<2);request(a,CAPS,&seid,1);
}
static void encoder_init(struct bt_a2dp *a)
{
    SBC_ENC_PARAMS *e=&a->encoder;memset(e,0,sizeof(*e));
    e->s16SamplingFreq=(a->config[0]&0x20)?SBC_sf44100:SBC_sf48000;
    e->s16ChannelMode=(a->config[0]&2)?SBC_STEREO:(a->config[0]&1)?SBC_JOINT_STEREO:SBC_MONO;
    e->s16NumOfSubBands=8;e->s16NumOfBlocks=16;
    e->s16AllocationMethod=(a->config[1]&1)?SBC_LOUDNESS:SBC_SNR;e->s16BitPool=a->config[2];
    SBC_Encoder_Init(e);
    a->status.rate=e->s16SamplingFreq==SBC_sf44100?44100:48000;
    a->status.channels=e->s16NumOfChannels;a->status.bitpool=e->s16BitPool;
}
static void response(struct bt_a2dp *a,unsigned signal,unsigned type,const uint8_t *p,unsigned n)
{
    if(signal!=a->pending)return;
    a->pending=0;
    if(type!=2){if(signal==CAPS){next_endpoint(a);return;}failed(a,GZ_ECONNREFUSED);return;}
    if(signal==DISCOVER){
        if(n%2){failed(a,GZ_ECORRUPT);return;}
        for(unsigned i=0;i<n;i+=2){unsigned seid=p[i]>>2;
#ifdef BT_SERIAL_DEBUG
            if(i<32)printf("bluetooth audio: SEP %u flags %02x type %02x\n",seid,p[i]&3,p[i+1]);
#endif
            if(seid && seid<=0x3e && !(p[i]&3) && p[i+1]==8 && a->endpoint_count<sizeof(a->endpoints))a->endpoints[a->endpoint_count++]=(uint8_t)seid;}
        next_endpoint(a);
    } else if(signal==CAPS){
        if(!capabilities(a,p,n)){next_endpoint(a);return;}
        uint8_t config[12]={(uint8_t)(a->seid<<2),4,1,0,7,6,0,0};memcpy(config+8,a->config,4);
        a->phase=2;request(a,CONFIGURE,config,sizeof(config));
    } else {
        if(n){failed(a,GZ_ECORRUPT);return;}
        uint8_t seid=(uint8_t)(a->seid<<2);
        if(signal==CONFIGURE){encoder_init(a);a->phase=3;request(a,OPEN,&seid,1);}
        else if(signal==OPEN){a->phase=4;bt_l2_open(a->link,2,0x19);}
        else if(signal==START){
            a->status.state=BT_AUDIO_STREAMING;a->link->status.state=BT_SERIAL_CONNECTED;
            a->last=a->link->now;a->clock=0;a->phase=6;
            memset(a->link->pin,0,sizeof(a->link->pin));a->link->pin_length=0;
        }
    }
}
static void peer_command(struct bt_a2dp *a,unsigned transaction,unsigned signal,const uint8_t *p,unsigned n)
{
    uint8_t reply[32]={(uint8_t)(transaction<<4|2),(uint8_t)signal};unsigned size=2;
    if(signal==DISCOVER && !n){reply[2]=4;reply[3]=0;size=4;}
    else if((signal==CAPS || signal==GET_CONFIG) && n==1 && p[0]==4){
        const uint8_t caps[]={1,0,7,6,0,0,0x3b,0x15,2,32};memcpy(reply+2,caps,sizeof(caps));size+=sizeof(caps);
        if(signal==GET_CONFIG)memcpy(reply+8,a->config,4);
    } else if((signal==SUSPEND || signal==CLOSE || signal==ABORT) && n==1 && p[0]==4){
        /* Release the ACL after acknowledging, keeping teardown asynchronous. */
        a->phase=7;a->deadline=a->link->now+100;a->tone_left=0;
    } else {
        reply[0]|=1;
        if(signal==START || signal==SUSPEND){reply[2]=n?p[0]:0;reply[3]=0x31;size=4;}
        else {reply[2]=0x31;size=3;} /* Bad state / unsupported peer operation. */
    }
    int error=bt_l2_send(a->link,1,reply,size);if(error)failed(a,error);
}
static void received(void *arg,unsigned index,const uint8_t *p,unsigned n)
{
    struct bt_a2dp *a=arg;if(n<1)return;
    if(index==3){int error=bt_source_sdp(a->link,3,p,n);if(error)failed(a,error);return;}
    unsigned type=p[0]&3,packet=(p[0]>>2)&3,transaction=p[0]>>4,signal;

#ifdef BT_SERIAL_DEBUG
    if(n<=40){
        printf("bluetooth audio: AVDTP RX ch %u",index);for(unsigned i=0;i<n;i++)printf(" %02x",p[i]);printf("\n");
    }

#endif
    if(index!=1)return;
    if(!packet){
        if(n<2){failed(a,GZ_ECORRUPT);return;}signal=p[1]&0x3f;p+=2;n-=2;
    } else if(packet==1){
        if(n<3 || p[1]<2 || a->fragment_left){failed(a,GZ_ECORRUPT);return;}
        a->fragment_transaction=p[0];a->fragment_left=(uint8_t)(p[1]-1);a->fragment_signal=p[2]&0x3f;a->fragment_length=0;
        p+=3;n-=3;
        if(n>sizeof(a->fragment)){failed(a,GZ_ECORRUPT);return;}
        memcpy(a->fragment,p,n);a->fragment_length=n;return;
    } else {
        if(!a->fragment_left || (p[0]&0xf3)!=(a->fragment_transaction&0xf3) ||
           (packet==3)!=(a->fragment_left==1) || n-1>sizeof(a->fragment)-a->fragment_length){failed(a,GZ_ECORRUPT);return;}
        memcpy(a->fragment+a->fragment_length,p+1,n-1);a->fragment_length+=n-1;
        if(--a->fragment_left)return;
        signal=a->fragment_signal;p=a->fragment;n=a->fragment_length;
    }
    if(!type)peer_command(a,transaction,signal,p,n);
    else if(transaction==a->transaction)response(a,signal,type,p,n);
}
void bt_a2dp_init(struct bt_a2dp *a,struct bt_serial *s)
{memset(a,0,sizeof(*a));a->link=s;}
int bt_a2dp_start(struct bt_a2dp *a,const uint8_t *peer,const uint8_t *pin,unsigned n,uint32_t id,uint32_t now)
{
    struct bt_serial *s=a->link;int error=bt_serial_start(s,peer,pin,n,id,now);if(error)return error;
    bt_a2dp_init(a,s);a->status.id=id;a->status.state=BT_AUDIO_CONNECTING;memcpy(a->status.peer,peer,6);
    s->service_uuid=0x110b;s->profile=a;s->profile_sdp=services;s->profile_ready=ready;s->profile_data=received;
    return 0;
}
int bt_a2dp_tone(struct bt_a2dp *a,unsigned ms)
{
    if(a->status.state!=BT_AUDIO_STREAMING)return GZ_ENOTCONN;
    if(ms>3000)return GZ_EINVAL;
    if(a->head!=a->tail)return GZ_EBUSY;
    a->tone_left=a->status.rate*ms/1000;a->tone_phase=0;return 0;
}
int bt_a2dp_write(struct bt_a2dp *a,const void *buffer,unsigned n)
{
    if(a->status.state!=BT_AUDIO_STREAMING)return GZ_ENOTCONN;
    unsigned align=a->status.channels*2;
    if(n%align)return GZ_EINVAL;
    if(a->tone_left)return GZ_EBUSY;
    unsigned count=MIN(n/2,(unsigned)(sizeof(a->ring)/2)-(a->head-a->tail));count-=count%a->status.channels;
    const int16_t *p=buffer;for(unsigned i=0;i<count;i++)a->ring[a->head++%(sizeof(a->ring)/2)]=p[i];
    a->status.queued=(a->head-a->tail)*2;return count?(int)(count*2):n?GZ_EBUSY:0;
}
void bt_a2dp_flush(struct bt_a2dp *a){a->head=a->tail=0;a->tone_left=0;a->status.queued=0;a->playing=false;}
void bt_a2dp_tick(struct bt_a2dp *a,uint32_t now)
{
    struct bt_serial *s=a->link;if(s->profile!=a)return;
    a->status.authenticated=s->status.authenticated;a->status.encrypted=s->status.encrypted;a->status.hci_status=s->status.hci_status;
    if(s->status.state==BT_SERIAL_FAILED){a->status.state=BT_AUDIO_FAILED;a->status.error=s->status.error;bt_a2dp_flush(a);return;}
    if(s->status.state==BT_SERIAL_IDLE){a->status.state=BT_AUDIO_IDLE;bt_a2dp_flush(a);return;}
    if(s->closing){a->status.state=BT_AUDIO_CLOSING;bt_a2dp_flush(a);return;}
    if(a->phase==7){if(due(now,a->deadline))bt_serial_close(s);return;}
    if(a->pending && due(now,a->deadline)){failed(a,GZ_ETIMEDOUT);return;}
    if(a->status.state!=BT_AUDIO_STREAMING){if(s->status.encrypted)a->status.state=BT_AUDIO_NEGOTIATING;return;}
    /* Accumulate a sample clock. Never build an unbounded catch-up backlog. */
    unsigned elapsed=MIN(now-a->last,100u);a->last=now;
    a->clock=MIN(a->clock+elapsed*a->status.rate,a->status.rate*100u);
    unsigned frame_bytes=4+4*a->status.channels+2*a->status.bitpool+(a->encoder.s16ChannelMode==SBC_JOINT_STEREO?1:0);
    /* Fit one controller ACL packet when possible; splitting a nearly-full
     * frame bundle would spend another reliable BCSP window per packet. */
    unsigned mtu=MIN(256u,(unsigned)s->channels[2].mtu);
    if(s->acl_mtu>=48)mtu=MIN(mtu,(unsigned)s->acl_mtu-4);
    if(mtu<13+frame_bytes){failed(a,GZ_EINVAL);return;}
    unsigned frames=MIN(15u,(mtu-13)/frame_bytes);
    if(a->clock<frames*128000u || s->qh-s->qt>=2)return;
    uint8_t packet[256]={0x80,96};packet[2]=(uint8_t)(a->sequence>>8);packet[3]=(uint8_t)a->sequence;
    packet[4]=(uint8_t)(a->timestamp>>24);packet[5]=(uint8_t)(a->timestamp>>16);packet[6]=(uint8_t)(a->timestamp>>8);packet[7]=(uint8_t)a->timestamp;
    packet[8]='G';packet[9]='L';packet[10]='A';packet[11]='Z';packet[12]=(uint8_t)frames;
    unsigned at=13,real_frames=0;
    for(unsigned i=0;i<frames;i++){
        unsigned samples=128*a->status.channels,available=a->playing?MIN(a->head-a->tail,samples):0;
        available-=available%a->status.channels;
        memset(a->pcm,0,sizeof(a->pcm));
        for(unsigned j=0;j<available;j++)a->pcm[j]=a->ring[a->tail++%(sizeof(a->ring)/2)];
        real_frames+=available/a->status.channels;
        if(a->playing && available<samples)a->status.underruns++;
        if(a->tone_left){
            for(unsigned j=0;j<128 && a->tone_left;j++,a->tone_left--){
                /* Quiet 440 Hz triangle, avoiding floating point and tables. */
                unsigned phase=a->tone_phase;a->tone_phase=(phase+440)%a->status.rate;
                int value=(int)(phase*4800/a->status.rate);value=value<2400?value-1200:3600-value;
                for(unsigned ch=0;ch<a->status.channels;ch++)a->pcm[j*a->status.channels+ch]=(int16_t)value;
            }
        }
        a->encoder.ps16PcmBuffer=a->pcm;a->encoder.pu8Packet=packet+at;SBC_Encoder(&a->encoder);
        if(a->encoder.u16PacketLength!=frame_bytes){failed(a,GZ_ECORRUPT);return;}at+=frame_bytes;
    }
    int error=bt_l2_send(s,2,packet,at);if(error){failed(a,error);return;}
    a->sequence++;a->timestamp+=frames*128;a->clock-=frames*128000u;
    a->status.frames_played+=real_frames;a->status.queued=(a->head-a->tail)*2;a->status.packets++;
}
