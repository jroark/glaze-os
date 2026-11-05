/* Classic SPP client: HCI legacy authentication/encryption, basic-mode L2CAP,
 * SDP ServiceSearchAttribute and RFCOMM TS 07.10 credit-based serial framing.
 * References: Bluetooth Core Vol 3 parts A/B and RFCOMM specification; see docs.
 * One peer is selected explicitly. Pairing keys live only in service memory.
 */
#include "serial.h"
#include <string.h>
#ifdef BT_SERIAL_DEBUG
#include <stdio.h>
#endif
static unsigned get16(const uint8_t *p) { return p[0]|(unsigned)p[1]<<8; }
static void put16(uint8_t *p,unsigned n) { p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); }
static unsigned be16(const uint8_t *p) { return (unsigned)p[0]<<8|p[1]; }
static bool due(uint32_t now,uint32_t end) { return (int32_t)(now-end)>=0; }
void bt_l2_fail(struct bt_serial *s,int error)
{
    s->status.state=BT_SERIAL_FAILED; s->status.error=error;
    s->qh=s->qt=s->offset=0; s->command_sent=false;s->closing=false;
    memset(s->pin,0,sizeof(s->pin)); s->pin_length=0;
    memset(s->security,0,sizeof(s->security)); s->security_opcode=0;
}
void bt_serial_init(struct bt_serial *s,int (*command)(void *,unsigned,const void *,unsigned),
                    int (*acl)(void *,const uint8_t *,unsigned),void *arg)
{
    memset(s,0,sizeof(*s)); s->command=command; s->acl=acl; s->arg=arg; s->handle=0xffff;
}
int bt_serial_start(struct bt_serial *s,const uint8_t *peer,const uint8_t *pin,unsigned n,uint32_t id,uint32_t now)
{
    if (s->status.state!=BT_SERIAL_IDLE && (s->status.state!=BT_SERIAL_FAILED || s->handle!=0xffff)) return GZ_EBUSY;
    if (!n || n>16) return GZ_EINVAL;
    uint8_t key[16],address[6]; bool valid=s->key_valid && !memcmp(peer,s->key_peer,6);
    memcpy(key,s->key,16); memcpy(address,s->key_peer,6);
    int (*command)(void *,unsigned,const void *,unsigned)=s->command;
    int (*acl)(void *,const uint8_t *,unsigned)=s->acl; void *arg=s->arg;uint16_t acl_mtu=s->acl_mtu;
    bt_serial_init(s,command,acl,arg);s->acl_mtu=acl_mtu;
    if (valid) { memcpy(s->key,key,16); memcpy(s->key_peer,address,6); s->key_valid=true; }
    memset(key,0,sizeof(key));
    memcpy(s->peer,peer,6); memcpy(s->status.peer,peer,6); memcpy(s->pin,pin,n); s->pin_length=(uint8_t)n;
    s->status.id=id; s->status.state=BT_SERIAL_CONNECTING; s->now=now; s->deadline=now+90000;
    s->rf_mtu=127;
    s->service_uuid=0x1101;
    for(unsigned i=0;i<4;i++){s->channels[i].local=(uint16_t)(0x40+i);s->channels[i].mtu=672;}
    return 0;
}
static int packet(struct bt_serial *s,unsigned cid,const uint8_t *p,unsigned n)
{
    if (n>256 || s->qh-s->qt==8) return GZ_EBUSY;
    unsigned index=s->qh++%8;
    put16(s->queue[index].bytes,n); put16(s->queue[index].bytes+2,cid);
    memcpy(s->queue[index].bytes+4,p,n); s->queue[index].length=(uint16_t)(n+4); return 0;
}
static void signal_packet(struct bt_serial *s,unsigned code,unsigned id,const uint8_t *p,unsigned n)
{
    uint8_t bytes[256];
    if (n>252) { bt_l2_fail(s,GZ_ECORRUPT); return; }
    bytes[0]=(uint8_t)code; bytes[1]=(uint8_t)id; put16(bytes+2,n); if(n)memcpy(bytes+4,p,n);
    if (packet(s,1,bytes,n+4)) bt_l2_fail(s,GZ_EFULL);
}
static unsigned next_id(struct bt_serial *s) { if(!++s->signal_id)++s->signal_id; return s->signal_id; }
void bt_l2_open(struct bt_serial *s,unsigned index,unsigned psm)
{
    if(index>=4){bt_l2_fail(s,GZ_EINVAL);return;}
    uint8_t p[4]; put16(p,psm); put16(p+2,s->channels[index].local);
    signal_packet(s,2,next_id(s),p,4);
}
static void configure(struct bt_serial *s,struct bt_channel *ch)
{
    uint8_t p[8]; put16(p,ch->remote); put16(p+2,0); p[4]=1;p[5]=2;put16(p+6,672);
    signal_packet(s,4,next_id(s),p,8);
}
static void sdp_query(struct bt_serial *s)
{
    uint8_t p[40]={6,0,0,0,0,0x35,3,0x19,0x11,1,2,0,0x35,6,9,0,4,9,1,0};
    p[8]=(uint8_t)(s->service_uuid>>8);p[9]=(uint8_t)s->service_uuid;
    if(!++s->sdp_id)++s->sdp_id;
    p[2]=s->sdp_id; p[20]=(uint8_t)s->continuation_length;
    memcpy(p+21,s->continuation,s->continuation_length);
    unsigned n=21+s->continuation_length; p[4]=(uint8_t)(n-5);
    if(packet(s,s->channels[0].remote,p,n))bt_l2_fail(s,GZ_EFULL);
}
/* Strict, depth-bounded SDP data-element traversal of the requested attribute 4. */
static int element(const uint8_t *p,unsigned size,unsigned *type,const uint8_t **value,unsigned *n,unsigned *used)
{
    if(!size)return GZ_ECORRUPT;
    unsigned index=p[0]&7,head=1,length=0; *type=p[0]>>3;
    if(index<5)length=1u<<index;
    else { unsigned bytes=1u<<(index-5); if(size<1+bytes)return GZ_ECORRUPT;
        for(unsigned i=0;i<bytes;i++) { if(length>BT_L2_MAX)return GZ_ECORRUPT; length=length<<8|p[1+i]; } head+=bytes;
    }
    if(*type==0)length=0;
    if(length>size-head)return GZ_ECORRUPT;
    *value=p+head;*n=length;*used=head+length;return 0;
}
static int find_rfcomm(const uint8_t *p,unsigned n,unsigned depth)
{
    if(depth>8)return GZ_ECORRUPT;
    while(n) {
        unsigned type,len,used;const uint8_t *value;
        if(element(p,n,&type,&value,&len,&used))return GZ_ECORRUPT;
        if(type==6) {
            unsigned first_type,first_len,first_used;const uint8_t *first;
            if(len && !element(value,len,&first_type,&first,&first_len,&first_used) && first_type==3 &&
                ((first_len==2 && be16(first)==3) || (first_len==4 && !first[0] && !first[1] && be16(first+2)==3) || (first_len==16 && !memcmp(first,"\0\0\0\3\0\0\20\0\200\0\0\200\137\233\64\373",16)))) {
                unsigned second_type,second_len,second_used;const uint8_t *second;
                if(!element(value+first_used,len-first_used,&second_type,&second,&second_len,&second_used) &&
                    second_type==1 && second_len==1 && second[0]>=1 && second[0]<=30)return second[0];
            }
            int found=find_rfcomm(value,len,depth+1);if(found)return found;
        }
        p+=used;n-=used;
    }
    return 0;
}
static int service_channel(const uint8_t *p,unsigned n)
{
    unsigned type,length,used;const uint8_t *outer;
    if(element(p,n,&type,&outer,&length,&used) || type!=6 || used!=n)return GZ_ECORRUPT;
    int fallback=0;
    while(length) {
        const uint8_t *record;unsigned record_length,record_used;
        if(element(outer,length,&type,&record,&record_length,&record_used) || type!=6)return GZ_ECORRUPT;
        int channel=0;bool preferred=false;
        while(record_length) {
            const uint8_t *id,*value;unsigned id_type,id_length,id_used,value_type,value_length,value_used;
            if(element(record,record_length,&id_type,&id,&id_length,&id_used) || id_type!=1 || id_length!=2)return GZ_ECORRUPT;
            if(element(record+id_used,record_length-id_used,&value_type,&value,&value_length,&value_used))return GZ_ECORRUPT;
            if(be16(id)==4)channel=find_rfcomm(record+id_used,value_used,0);
            if(be16(id)==0x100 && value_type==4 && value_length==17 && !memcmp(value,"Glaze Serial Echo",17))preferred=true;
            record+=id_used+value_used;record_length-=id_used+value_used;
        }
        if(channel<0)return channel;
        if(channel && preferred)return channel;
        if(channel && !fallback)fallback=channel;
        outer+=record_used;length-=record_used;
    }
    return fallback;
}

static uint8_t fcs(const uint8_t *p,unsigned n)
{
    uint8_t crc=0xff;
    for(unsigned i=0;i<n;i++){crc^=p[i];for(unsigned j=0;j<8;j++)crc=(uint8_t)((crc>>1)^((crc&1)?0xe0:0));}
    return (uint8_t)(0xff-crc);
}
static int rf_frame(struct bt_serial *s,unsigned dlci,unsigned control,bool command,const uint8_t *p,unsigned n,int credits)
{
    if(n>127)return GZ_EINVAL;
    uint8_t bytes[134]; unsigned at=0;
    bytes[at++]=(uint8_t)(dlci<<2|(command?2:0)|1); bytes[at++]=(uint8_t)control; bytes[at++]=(uint8_t)(n<<1|1);
    if(credits>=0)bytes[at++]=(uint8_t)credits;
    if(n)memcpy(bytes+at,p,n);
    at+=n;
    bytes[at]=fcs(bytes,(control&0xef)==0xef?2:3);at++;
    return packet(s,s->channels[1].remote,bytes,at);
}
static void rf_control(struct bt_serial *s,unsigned type,bool command,const uint8_t *p,unsigned n)
{
    uint8_t bytes[32]; if(n>30){bt_l2_fail(s,GZ_ECORRUPT);return;}
    bytes[0]=(uint8_t)(type|(command?2:0)|1);bytes[1]=(uint8_t)(n<<1|1);memcpy(bytes+2,p,n);
    if(rf_frame(s,0,0xef,true,bytes,n+2,-1))bt_l2_fail(s,GZ_EFULL);
}
static void channel_ready(struct bt_serial *s,unsigned index)
{
    struct bt_channel *ch=&s->channels[index]; if(!ch->configured || !ch->peer_configured)return;
    if(!index && s->stage==3){s->stage=4;sdp_query(s);}
    if(index && s->profile_ready){s->profile_ready(s->profile,index);return;}
    if(index && s->stage==5){s->stage=6;if(rf_frame(s,0,0x3f,true,NULL,0,-1))bt_l2_fail(s,GZ_EFULL);}
}
static void l2_signal(struct bt_serial *s,const uint8_t *p,unsigned n)
{
    while(n>=4) {
        unsigned code=p[0],id=p[1],length=get16(p+2);p+=4;n-=4;
        if(!id || length>n){bt_l2_fail(s,GZ_ECORRUPT);return;}
#ifdef BT_SERIAL_DEBUG
        if(s->profile && code==2 && length>=4)printf("bluetooth audio: incoming PSM %x CID %x\n",get16(p),get16(p+2));
#endif
        if(code==2 && length==4){
            unsigned psm=get16(p),remote=get16(p+2);struct bt_channel *ch=&s->channels[3];
            unsigned result=psm==1 && s->profile && s->status.encrypted?0:2;
            if(!result && (remote<0x40 || ch->remote))result=4;
            for(unsigned i=0;i<3;i++)if(!result && s->channels[i].remote==remote)result=4;
            uint8_t reply[8]={0};put16(reply,result?0:ch->local);put16(reply+2,remote);put16(reply+4,result);
            signal_packet(s,3,id,reply,sizeof(reply));
            if(!result){ch->remote=(uint16_t)remote;configure(s,ch);}
        } else if(code==3 && length>=8) {
            unsigned local=get16(p+2),result=get16(p+4);
            for(unsigned i=0;i<4;i++)if(s->channels[i].local==local) {
                if(result==1)break;
                if(result || get16(p)<0x40){bt_l2_fail(s,GZ_ECONNREFUSED);return;}
                s->channels[i].remote=(uint16_t)get16(p);configure(s,&s->channels[i]);break;
            }
        } else if(code==4 && length>=4) {
            struct bt_channel *ch=NULL;unsigned index=0;
            for(unsigned i=0;i<4;i++)if(s->channels[i].local==get16(p)){ch=&s->channels[i];index=i;break;}
            if(!ch){uint8_t reject[2]={2,0};signal_packet(s,1,id,reject,2);}
            else {
                uint8_t reply[128];put16(reply,ch->remote);put16(reply+2,0);put16(reply+4,0);unsigned rlen=6;
                if(get16(p+2)){put16(reply+4,2);}
                for(unsigned at=4;at<length;) {
                    if(length-at<2 || p[at+1]>length-at-2){bt_l2_fail(s,GZ_ECORRUPT);return;}
                    unsigned type=p[at]&0x7f,len=p[at+1];
                    if(type==1 && len==2){unsigned mtu=get16(p+at+2);if(mtu<48){put16(reply+4,1);reply[6]=1;reply[7]=2;put16(reply+8,48);rlen=10;}else ch->mtu=(uint16_t)mtu;}
                    else if(type==2 && len==2){} /* optional flush timeout */
                    else if(!(p[at]&0x80)){put16(reply+4,3);if(rlen+2+len>sizeof(reply)){bt_l2_fail(s,GZ_ECORRUPT);return;}memcpy(reply+rlen,p+at,len+2);rlen+=len+2;}
                    at+=len+2;
                }
                signal_packet(s,5,id,reply,rlen);
                if(!get16(reply+4)){ch->peer_configured=1;channel_ready(s,index);}
            }
        } else if(code==5 && length>=6) {
            for(unsigned i=0;i<4;i++)if(s->channels[i].local==get16(p)) {
                if(get16(p+2) || get16(p+4)){bt_l2_fail(s,GZ_ECONNREFUSED);return;}
                s->channels[i].configured=1;channel_ready(s,i);
            }
        } else if(code==0x0a && length==2) {
            uint8_t reply[12]={0};unsigned type=get16(p);put16(reply,type);unsigned count=4;
            if(type==1){put16(reply+4,672);count=6;}
            else if(type==2)count=8;
            else if(type==3){reply[4]=2;count=12;}
            else put16(reply+2,1);
            signal_packet(s,0x0b,id,reply,count);
        } else if(code==6 && length==4) {
            signal_packet(s,7,id,p,4);
            if(s->profile && (get16(p)==s->channels[3].local || (get16(p)==s->channels[0].local && s->stage>=5))){
                unsigned index=get16(p)==s->channels[3].local?3:0;
                s->channels[index].remote=0;s->channels[index].configured=s->channels[index].peer_configured=0;
            } else bt_l2_fail(s,GZ_ENOTCONN);
        }
        else if(code==8)signal_packet(s,9,id,p,length);
        else if(code!=1 && code!=7 && code!=9 && code!=0x0b){uint8_t reject[2]={0};signal_packet(s,1,id,reject,2);}
        p+=length;n-=length;
    }
    if(n)bt_l2_fail(s,GZ_ECORRUPT);
}
static void sdp_response(struct bt_serial *s,const uint8_t *p,unsigned n)
{
    if(n<8 || p[0]!=7 || be16(p+1)!=s->sdp_id || be16(p+3)!=n-5){bt_l2_fail(s,GZ_ECORRUPT);return;}
    unsigned count=be16(p+5);
    if(count>n-8 || s->attribute_length+count>sizeof(s->attributes)){bt_l2_fail(s,GZ_ECORRUPT);return;}
    unsigned continuation=p[7+count];
    if(continuation>16 || n!=8+count+continuation){bt_l2_fail(s,GZ_ECORRUPT);return;}
    memcpy(s->attributes+s->attribute_length,p+7,count);s->attribute_length+=count;
    if(continuation){memcpy(s->continuation,p+8+count,continuation);s->continuation_length=continuation;sdp_query(s);return;}
#ifdef BT_SERIAL_DEBUG
    printf("bt-sdp: attributes %u:",s->attribute_length);
    for(unsigned i=0;i<s->attribute_length;i++)printf(" %02x",s->attributes[i]);
    printf("\n");
#endif
    if(s->profile_sdp){s->profile_sdp(s->profile,s->attributes,s->attribute_length);return;}
    int channel=service_channel(s->attributes,s->attribute_length);
    if(channel<1 || channel>30){bt_l2_fail(s,GZ_ENOENT);return;}
    s->status.channel=(uint8_t)channel;s->dlci=(uint8_t)(channel<<1);s->stage=5;
    s->status.state=BT_SERIAL_OPENING;bt_l2_open(s,1,3);
}
static void rf_mcc(struct bt_serial *s,const uint8_t *p,unsigned n)
{
    if(n<2 || !(p[0]&1) || !(p[1]&1) || n!=2u+(p[1]>>1)){bt_l2_fail(s,GZ_ECORRUPT);return;}
    unsigned type=p[0]&0xfc,length=p[1]>>1;bool command=(p[0]&2)!=0;p+=2;
    if(type==0x80 && length==8 && p[0]==s->dlci) {
        uint8_t pn[8];memcpy(pn,p,8);
        unsigned mtu=get16(p+4);if(mtu<1){bt_l2_fail(s,GZ_ECORRUPT);return;}
        s->rf_mtu=(uint16_t)MIN(mtu,MIN(127u,(unsigned)s->channels[1].mtu-5));s->credit_flow=command?p[1]==0xf0:p[1]==0xe0;
        s->tx_credits=p[7];s->rx_credits=7;
        if(command){pn[1]=s->credit_flow?0xe0:0;put16(pn+4,s->rf_mtu);pn[7]=7;rf_control(s,0x80,false,pn,8);}
        else if(s->stage==7){s->stage=8;if(rf_frame(s,s->dlci,0x3f,true,NULL,0,-1))bt_l2_fail(s,GZ_EFULL);}
    } else if(type==0xe0 && length>=2 && (p[0]>>2)==s->dlci) {
        s->remote_paused=(p[1]&2)!=0;
        if(command)rf_control(s,type,false,p,length);
        else if(s->stage==9){s->stage=10;s->status.state=BT_SERIAL_CONNECTED;memset(s->pin,0,sizeof(s->pin));s->pin_length=0;}
    } else if(type==0xa0 || type==0x60){s->remote_paused=type==0x60;if(command)rf_control(s,type,false,p,length);}
    else if(command && type==0x90 && (length==1 || length==8)) {uint8_t rpn[8]={p[0],3,3,0,0x11,0x13,0x7f,0x3f};if(length==8)memcpy(rpn,p,8);rf_control(s,type,false,rpn,8);}
    else if(command && (type==0x20 || type==0x50))rf_control(s,type,false,p,length);
    else if(command){uint8_t unsupported=(uint8_t)(type|3);rf_control(s,0x10,false,&unsupported,1);}
}
static void rf_receive(struct bt_serial *s,const uint8_t *p,unsigned n)
{
    if(n<4 || !(p[0]&1)){bt_l2_fail(s,GZ_ECORRUPT);return;}
    unsigned dlci=p[0]>>2,control=p[1]&0xef,head=3,length=p[2]>>1;
    if(!(p[2]&1)){if(n<5){bt_l2_fail(s,GZ_ECORRUPT);return;}length|=(unsigned)p[3]<<7;head++;}
    unsigned credit=(control==0xef && (p[1]&0x10) && dlci && s->credit_flow)?1:0;
    if(n!=head+credit+length+1 || p[n-1]!=fcs(p,control==0xef?2:head)){bt_l2_fail(s,GZ_ECORRUPT);return;}
    if(control==0x63) {
        if(!dlci && s->stage==6){s->stage=7;uint8_t pn[8]={s->dlci,0xf0,0,0,127,0,0,7};put16(pn+4,MIN(127u,(unsigned)s->channels[1].mtu-5));rf_control(s,0x80,true,pn,8);}
        else if(dlci==s->dlci && s->stage==8){s->stage=9;uint8_t msc[2]={(uint8_t)(s->dlci<<2|3),0x8d};rf_control(s,0xe0,true,msc,2);}
    } else if(control==0x2f && dlci==s->dlci){if(rf_frame(s,dlci,0x73,false,NULL,0,-1))bt_l2_fail(s,GZ_EFULL);}
    else if(control==0x43){rf_frame(s,dlci,0x73,false,NULL,0,-1);bt_l2_fail(s,GZ_ENOTCONN);}
    else if(control==0x0f)bt_l2_fail(s,GZ_ECONNREFUSED);
    else if(control==0xef && !dlci)rf_mcc(s,p+head,length);
    else if(control==0xef && dlci==s->dlci) {
        if(credit)s->tx_credits=(uint8_t)MIN(255u,(unsigned)s->tx_credits+p[head]);
        if(length) {
            if(length>s->rf_mtu || length>sizeof(s->rx)-(s->rh-s->rt) || (s->credit_flow&&!s->rx_credits)){bt_l2_fail(s,GZ_EFULL);return;}
            if(s->credit_flow)s->rx_credits--;
            for(unsigned i=0;i<length;i++)s->rx[s->rh++%sizeof(s->rx)]=p[head+credit+i];
            s->status.received+=length;s->status.available=s->rh-s->rt;
        }
    }
}
void bt_serial_acl(struct bt_serial *s,const uint8_t *p,unsigned n)
{
    if(s->status.state==BT_SERIAL_IDLE || s->status.state==BT_SERIAL_FAILED || s->status.state==BT_SERIAL_CLOSING)return;
    if(n<4 || n!=4u+get16(p+2) || (get16(p)&0xfff)!=s->handle)return;
    unsigned boundary=(p[1]>>4)&3;p+=4;n-=4;
    if(boundary==0 || boundary==2){
        if(n<4 || get16(p)>BT_L2_MAX){bt_l2_fail(s,GZ_ECORRUPT);return;}
        s->have=0;s->want=4+get16(p);
    } else if(boundary!=1 || !s->have)return;
    if(n>s->want-s->have){bt_l2_fail(s,GZ_ECORRUPT);return;}
    memcpy(s->reassembly+s->have,p,n);s->have+=n;
    if(s->have!=s->want)return;
    const uint8_t *data=s->reassembly+4;unsigned length=s->want-4,cid=get16(s->reassembly+2);
    if(cid==1)l2_signal(s,data,length);
    else if(cid==s->channels[0].local && s->stage==4)sdp_response(s,data,length);
    else if(s->profile_data){for(unsigned i=1;i<4;i++)if(cid==s->channels[i].local)s->profile_data(s->profile,i,data,length);}
    else if(cid==s->channels[1].local)rf_receive(s,data,length);
    s->have=s->want=0;
}
bool bt_serial_opcode(unsigned op)
{ return op==0x0405 || op==0x0406 || op==0x0408 || (op>=0x040b && op<=0x040e) || op==0x0411 || op==0x0413; }
void bt_serial_event(struct bt_serial *s,const uint8_t *p,unsigned n)
{
    if(n<2 || n!=2u+p[1])return;
    if(p[0]==3 && n==13 && !memcmp(p+5,s->peer,6)) {
        if(p[2]){s->status.hci_status=p[2];if(s->status.state!=BT_SERIAL_IDLE)bt_l2_fail(s,GZ_ECONNREFUSED);return;}
        s->handle=(uint16_t)get16(p+3);
        if(s->status.state==BT_SERIAL_IDLE || s->closing){s->status.state=BT_SERIAL_CLOSING;s->closing=true;s->command_sent=false;return;}
        s->status.state=BT_SERIAL_PAIRING;s->command_sent=false;
    } else if((p[0]==0x16 || p[0]==0x17) && n==8 && !memcmp(p+2,s->peer,6)) {
        if(s->security_opcode){bt_l2_fail(s,GZ_EFULL);return;}
        memcpy(s->security,s->peer,6);s->security_length=6;
        if(p[0]==0x16 && s->pin_length){s->security_opcode=0x040d;s->security[6]=s->pin_length;memset(s->security+7,0,16);memcpy(s->security+7,s->pin,s->pin_length);s->security_length=23;}
        else if(p[0]==0x17 && s->key_valid && !memcmp(s->peer,s->key_peer,6)){s->security_opcode=0x040b;memcpy(s->security+6,s->key,16);s->security_length=22;}
        else s->security_opcode=p[0]==0x16?0x040e:0x040c;
    } else if(p[0]==0x18 && n==25 && !memcmp(p+2,s->peer,6)) {
        memcpy(s->key_peer,p+2,6);memcpy(s->key,p+8,16);s->key_valid=true;
    } else if(p[0]==6 && n==5 && get16(p+3)==s->handle) {
        if(p[2]){s->status.hci_status=p[2];s->key_valid=false;memset(s->key,0,16);bt_l2_fail(s,GZ_EPERM);return;}
        s->status.authenticated=1;
        if(s->status.state==BT_SERIAL_PAIRING){s->status.state=BT_SERIAL_ENCRYPTING;s->command_sent=false;}
    } else if(p[0]==8 && n==6 && get16(p+3)==s->handle) {
        if(p[2] || !p[5]){s->status.hci_status=p[2];bt_l2_fail(s,GZ_EPERM);return;}
        s->status.encrypted=1;
        if(s->status.state==BT_SERIAL_ENCRYPTING){
            if(s->status.channel){s->status.state=BT_SERIAL_OPENING;s->stage=5;s->dlci=(uint8_t)(s->status.channel<<1);bt_l2_open(s,1,3);}
            else {s->status.state=BT_SERIAL_DISCOVERING;s->stage=3;bt_l2_open(s,0,1);}
        }
    } else if(p[0]==5 && n==6 && get16(p+3)==s->handle) {
        s->handle=0xffff;s->qh=s->qt=s->offset=0;
        if(s->closing){s->status.state=BT_SERIAL_IDLE;s->closing=false;}
        else if(s->status.state!=BT_SERIAL_FAILED)bt_l2_fail(s,GZ_ENOTCONN);
        memset(s->pin,0,16);s->pin_length=0;
    } else if((p[0]==0x0e || p[0]==0x0f) && n>=6) {
        unsigned op=p[0]==0x0e?get16(p+3):get16(p+4),status=p[0]==0x0e?p[5]:p[2];
        if(status && bt_serial_opcode(op)){s->status.hci_status=(uint8_t)status;bt_l2_fail(s,GZ_EIO);}
        if(!status && op==0x0408){s->status.state=BT_SERIAL_IDLE;s->closing=false;memset(s->pin,0,16);s->pin_length=0;}
    }
}
static void transmit(struct bt_serial *s)
{
    if(s->qt==s->qh || s->handle==0xffff)return;
    unsigned index=s->qt%8,remain=s->queue[index].length-s->offset;
    unsigned count=MIN(remain,(unsigned)s->acl_mtu);if(!count)return;
    uint8_t p[258];put16(p,s->handle|(s->offset?0x1000:0x2000));put16(p+2,count);
    memcpy(p+4,s->queue[index].bytes+s->offset,count);
    int r=s->acl(s->arg,p,count+4);
    if(!r){
#ifdef BT_SERIAL_DEBUG
        if(s->profile && !s->offset && get16(s->queue[index].bytes+2)==s->channels[1].remote && s->queue[index].length>=6){
            const uint8_t *v=s->queue[index].bytes+4;
            printf("bluetooth audio: sent %02x %02x %02x len %u CID %x\n",v[0],v[1],s->queue[index].length>6?v[2]:0,s->queue[index].length-4,get16(s->queue[index].bytes+2));
        }
#endif
        s->offset+=count;if(s->offset==s->queue[index].length){s->qt++;s->offset=0;}}
    else if(r!=GZ_EBUSY)bt_l2_fail(s,r);
}
void bt_serial_tick(struct bt_serial *s,uint32_t now)
{
    s->now=now;
    if(s->status.state==BT_SERIAL_IDLE)return;
    if(s->status.state!=BT_SERIAL_CONNECTED && s->status.state!=BT_SERIAL_FAILED && due(now,s->deadline))bt_l2_fail(s,GZ_ETIMEDOUT);
    if(s->security_opcode){
        if(!s->command(s->arg,s->security_opcode,s->security,s->security_length)){s->security_opcode=0;memset(s->security,0,sizeof(s->security));}
        return;
    }
    if(s->status.state==BT_SERIAL_FAILED || s->closing){
        if(s->command_sent || (s->status.state==BT_SERIAL_FAILED&&s->handle==0xffff))return;
        uint8_t p[6];unsigned op,n;
        if(s->handle==0xffff){op=0x0408;memcpy(p,s->peer,6);n=6;}
        else {op=0x0406;put16(p,s->handle);p[2]=0x13;n=3;}
        if(!s->command(s->arg,op,p,n))s->command_sent=true;
        return;
    }
    if(!s->command_sent && (s->status.state==BT_SERIAL_CONNECTING || s->status.state==BT_SERIAL_PAIRING || s->status.state==BT_SERIAL_ENCRYPTING)) {
        uint8_t p[13]={0};unsigned op,n;
        if(s->status.state==BT_SERIAL_CONNECTING){op=0x0405;memcpy(p,s->peer,6);p[6]=0x18;p[7]=0xcc;p[8]=1;p[12]=1;n=13;}
        else {op=s->status.state==BT_SERIAL_PAIRING?0x0411:0x0413;put16(p,s->handle);p[2]=1;n=op==0x0413?3:2;}
        if(!s->command(s->arg,op,p,n))s->command_sent=true;
    }
    transmit(s);
    if(s->status.state==BT_SERIAL_CONNECTED && !s->profile) {
        unsigned free=sizeof(s->rx)-(s->rh-s->rt),budget=MIN(7u,free/s->rf_mtu);
        if(s->credit_flow && budget>s->rx_credits) {
            unsigned grant=budget-s->rx_credits;
            if(!rf_frame(s,s->dlci,0xff,true,NULL,0,(int)grant))s->rx_credits+=(uint8_t)grant;
        }
        if(s->tt!=s->th && !s->remote_paused && (!s->credit_flow || s->tx_credits)) {
            uint8_t p[127];unsigned n=MIN(s->th-s->tt,(unsigned)s->rf_mtu);
            for(unsigned i=0;i<n;i++)p[i]=s->tx[(s->tt+i)%sizeof(s->tx)];
            if(!rf_frame(s,s->dlci,0xef,true,p,n,-1)){s->tt+=n;s->status.sent+=n;if(s->credit_flow)s->tx_credits--;}
        }
    }
}
int bt_serial_close(struct bt_serial *s)
{
    if(s->status.state==BT_SERIAL_IDLE)return 0;
    if(s->status.state==BT_SERIAL_FAILED && s->handle==0xffff){s->status.state=BT_SERIAL_IDLE;return 0;}
    s->closing=true;s->command_sent=false;s->deadline=s->now+10000;s->status.state=BT_SERIAL_CLOSING;return 0;
}
int bt_serial_tx(struct bt_serial *s,const uint8_t *p,unsigned n)
{
    if(s->status.state!=BT_SERIAL_CONNECTED)return GZ_ENOTCONN;
    unsigned count=MIN(n,sizeof(s->tx)-(s->th-s->tt));
    for(unsigned i=0;i<count;i++)s->tx[s->th++%sizeof(s->tx)]=p[i];
    return count?(int)count:n?GZ_EBUSY:0;
}
int bt_serial_rx(struct bt_serial *s,uint8_t *p,unsigned n)
{
    unsigned count=MIN(n,s->rh-s->rt);
    for(unsigned i=0;i<count;i++)p[i]=s->rx[s->rt++%sizeof(s->rx)];
    s->status.available=s->rh-s->rt;
    return count?(int)count:s->status.state==BT_SERIAL_CONNECTED?0:GZ_ENOTCONN;
}

int bt_l2_send(struct bt_serial *s,unsigned index,const uint8_t *p,unsigned n)
{
    if(index>=4 || !s->channels[index].remote)return GZ_ENOTCONN;
    if(n>s->channels[index].mtu)return GZ_EINVAL;
    return packet(s,s->channels[index].remote,p,n);
}
