/* SDP 1.0 server for the local A2DP AudioSource. Speakers commonly query the
 * source's profile version/features while establishing their audio session.
 * The one immutable record fits bounded storage; continuation obeys both the
 * peer's requested byte count and its negotiated L2CAP MTU.
 */
#include "source_sdp.h"
#include <string.h>
#include <stdio.h>
static unsigned big16(const uint8_t *p){return (unsigned)p[0]<<8|p[1];}
static void big_put(uint8_t *p,unsigned v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static const uint8_t record[]={
    9,0,0,0x0a,0,1,0,1,
    9,0,1,0x35,3,0x19,0x11,0x0a,
    9,0,4,0x35,16,0x35,6,0x19,1,0,9,0,0x19,0x35,6,0x19,0,0x19,9,1,0,
    9,0,5,0x35,3,0x19,0x10,2,
    9,0,9,0x35,8,0x35,6,0x19,0x11,0x0d,9,1,0,
    9,1,0,0x25,11,'G','l','a','z','e',' ','A','u','d','i','o',
    9,3,0x11,9,0,1
};
_Static_assert(sizeof(record)<252,"SDP record fits short sequences");
struct elem {unsigned type,n,used;const uint8_t *p;};
static bool element(const uint8_t *p,unsigned n,struct elem *e)
{
    if(!n)return false;
    unsigned index=p[0]&7,head=1,len=0;e->type=p[0]>>3;
    if(index<5)len=1u<<index;
    else {unsigned bytes=1u<<(index-5);if(n<1+bytes)return false;
        for(unsigned i=0;i<bytes;i++){if(len>1024)return false;len=len<<8|p[1+i];}head+=bytes;}
    if(!e->type)len=0;
    if(head>n || len>n-head)return false;
    e->p=p+head;e->n=len;e->used=head+len;return true;
}
static int search(const struct elem *list)
{
    if(list->type!=6 || !list->n)return -1;
    bool match=true;const uint8_t *p=list->p;unsigned n=list->n;
    while(n){
        struct elem e;if(!element(p,n,&e) || e.type!=3)return -1;
        unsigned uuid=0;
        if(e.n==2)uuid=big16(e.p);
        else if(e.n==4 && !e.p[0] && !e.p[1])uuid=big16(e.p+2);
        else if(e.n==16 && !e.p[0] && !e.p[1] && !memcmp(e.p+4,"\0\0\20\0\200\0\0\200\137\233\64\373",12))uuid=big16(e.p+2);
        if(uuid!=0x110a && uuid!=0x110d && uuid!=0x1002 && uuid!=0x100 && uuid!=0x19)match=false;
        p+=e.used;n-=e.used;
    }
    return match?1:0;
}
static int attributes(const struct elem *list,uint8_t *out)
{
    if(list->type!=6 || !list->n)return -1;
    /* Validate the complete filter, including entries which match no ID. */
    const uint8_t *p=list->p;unsigned n=list->n;
    while(n){struct elem e;if(!element(p,n,&e) || e.type!=1 || (e.n!=2 && e.n!=4) || (e.n==4 && big16(e.p)>big16(e.p+2)))return -1;p+=e.used;n-=e.used;}
    unsigned at=0;
    for(unsigned off=0;off<sizeof(record);){
        struct elem value;if(sizeof(record)-off<3 || !element(record+off+3,sizeof(record)-off-3,&value))return -1;
        unsigned id=big16(record+off+1),size=3+value.used;bool include=false;p=list->p;n=list->n;
        while(n){struct elem e;if(!element(p,n,&e))return -1;
            unsigned low=big16(e.p),high=e.n==4?big16(e.p+2):low;if(id>=low && id<=high)include=true;
            p+=e.used;n-=e.used;}
        if(include){memcpy(out+at,record+off,size);at+=size;}off+=size;
    }
    return (int)at;
}
static int send_error(struct bt_serial *s,unsigned index,const uint8_t *p,unsigned error)
{uint8_t out[]={1,p[1],p[2],0,2,0,(uint8_t)error};return bt_l2_send(s,index,out,sizeof(out));}
int bt_source_sdp(struct bt_serial *s,unsigned index,const uint8_t *p,unsigned n)
{
    if(n<5)return GZ_ECORRUPT;
    if(big16(p+3)!=n-5)return send_error(s,index,p,4);
    unsigned op=p[0],off=5;int found=1;struct elem filter;
    if(op==2 || op==6){
        struct elem pattern;if(!element(p+off,n-off,&pattern))return send_error(s,index,p,3);
        found=search(&pattern);if(found<0)return send_error(s,index,p,3);off+=pattern.used;
    } else if(op==4){
        if(n-off<4)return send_error(s,index,p,4);
        if(memcmp(p+off,"\0\1\0\1",4))return send_error(s,index,p,2);
        off+=4;
    } else return send_error(s,index,p,3);
    if(n-off<3)return send_error(s,index,p,4);
    unsigned maximum=big16(p+off);off+=2;
    if(op==2){
        if(!maximum || n-off!=1 || p[off])return send_error(s,index,p,p[off]?5:3);
        uint8_t out[14]={3,p[1],p[2],0,5,0,0,0,0,0,1,0,1,0};
        unsigned count=10;
        if(found){out[4]=9;out[6]=out[8]=1;count=14;}
        return bt_l2_send(s,index,out,count);
    }
    if(maximum<7 || !element(p+off,n-off,&filter))return send_error(s,index,p,3);
    off+=filter.used;
    if(off>=n)return send_error(s,index,p,4);
    unsigned continuation=p[off++],start=0;
    if((continuation!=0 && continuation!=2) || n-off!=continuation)return send_error(s,index,p,5);
    if(continuation)start=big16(p+off);
    uint8_t selected[sizeof(record)],blob[sizeof(record)+4];int size=attributes(&filter,selected);
    if(size<0)return send_error(s,index,p,3);
    unsigned total;
    if(op==6){blob[0]=0x35;blob[1]=found?(uint8_t)(size+2):0;total=2;
        if(found){blob[2]=0x35;blob[3]=(uint8_t)size;memcpy(blob+4,selected,(unsigned)size);total+=(unsigned)size+2;}}
    else {blob[0]=0x35;blob[1]=(uint8_t)size;memcpy(blob+2,selected,(unsigned)size);total=(unsigned)size+2;}
    if(start>=total)return send_error(s,index,p,5);
    unsigned mtu=MIN(256u,(unsigned)s->channels[index].mtu);
    if(mtu<16)return GZ_EINVAL;
    unsigned count=MIN(total-start,MIN(maximum,mtu-10)),end=start+count;
    uint8_t out[256]={(uint8_t)(op==6?7:5),p[1],p[2],0,0};big_put(out+5,count);memcpy(out+7,blob+start,count);
    unsigned used=8+count;out[7+count]=0;
    if(end<total){out[7+count]=2;big_put(out+8+count,end);used+=2;}
    big_put(out+3,used-5);
    printf("bluetooth audio: source SDP op %u match %d bytes %u/%u\n",op,found,end,total);
    return bt_l2_send(s,index,out,used);
}
