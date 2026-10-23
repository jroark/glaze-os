/* Integer SN76489: 16-clock divider, three square channels and 16-bit LFSR.
 * Timestamped writes preserve mid-frame frequency/volume changes. */
#include "../../third_party/totalsms/sn76489.h"
#include <stdlib.h>
#include <string.h>
struct Sn76489 {
    uint32_t clock,rate,time,phase,divider;
    unsigned period[3],counter[4],level[4],attenuation[4],gain[4],master;
    unsigned latched,noise,taps,feed,lfsr,stereo;
    int16_t samples[4096]; unsigned count,read;
};
static const int volume[16]={8191,6506,5168,4105,3261,2590,2057,1634,1298,1031,819,650,517,410,326,0};
Sn76489 *psg_init(uint32_t clock,uint32_t rate) {
    Sn76489 *p=calloc(1,sizeof *p);
    if (p) { p->clock=clock; p->rate=rate; psg_reset(p,9,15); }
    return p;
}
void psg_quit(Sn76489 *p) { free(p); }
void psg_reset(Sn76489 *p,unsigned taps,unsigned feed) {
    uint32_t clock=p->clock,rate=p->rate;
    memset(p,0,sizeof *p); p->clock=clock; p->rate=rate;
    p->taps=taps; p->feed=feed; p->lfsr=1u<<feed; p->master=128; p->stereo=255;
    for(unsigned i=0;i<4;i++) { p->attenuation[i]=15; p->gain[i]=256; p->counter[i]=1; }
}
static void tick(Sn76489 *p) {
    for(unsigned i=0;i<3;i++) if (!--p->counter[i]) {
        p->counter[i]=p->period[i] ? p->period[i] : 1024; p->level[i]^=1;
    }
    if (!--p->counter[3]) {
        p->counter[3]=(p->noise&3)==3 ? (p->period[2] ? p->period[2] : 1024)*2 : (32u<<(p->noise&3));
        unsigned bits=p->lfsr & ((p->noise&4) ? p->taps : 1u),feedback=0;
        while(bits) { feedback^=bits&1; bits>>=1; }
        p->lfsr=(p->lfsr>>1)|(feedback<<p->feed); p->level[3]=p->lfsr&1;
    }
}
static void sync_to(Sn76489 *p,uint32_t time) {
    uint32_t delta=time-p->time;
    if (delta>0x40000000u) return;
    while (delta) {
        uint32_t step=(p->clock-p->phase+p->rate-1)/p->rate;
        if (step>delta) step=delta;
        p->divider+=step;
        while(p->divider>=16) { tick(p); p->divider-=16; }
        p->phase+=step*p->rate; p->time+=step; delta-=step;
        if (p->phase>=p->clock) {
            p->phase-=p->clock;
            int sum[2]={0,0};
            for(unsigned ch=0;ch<4;ch++) {
                int v=(p->level[ch] ? 1 : -1)*volume[p->attenuation[ch]]*(int)p->gain[ch]/256;
                if(p->stereo&(1u<<(ch+4))) sum[0]+=v;
                if(p->stereo&(1u<<ch)) sum[1]+=v;
            }
            for(unsigned lr=0;lr<2;lr++) {
                int v=sum[lr]*(int)p->master/256;
                if(v>32767)v=32767;
                if(v< -32768)v= -32768;
                if(p->count<2048)p->samples[p->count*2+lr]=v;
            }
            if(p->count<2048)p->count++;
        }
    }
}
void psg_write_io(Sn76489 *p,unsigned v,unsigned time) {
    sync_to(p,time);
    if (v&128) p->latched=(v>>4)&7;
    unsigned ch=p->latched>>1;
    if (p->latched&1) p->attenuation[ch]=v&15;
    else if (ch==3) { p->noise=v&7; p->lfsr=1u<<p->feed; }
    else if (v&128) p->period[ch]=(p->period[ch]&1008)|(v&15);
    else p->period[ch]=(p->period[ch]&15)|((v&63)<<4);
}
void psg_gg_write_io(Sn76489 *p,unsigned v,unsigned t) { sync_to(p,t); p->stereo=v; }
void psg_set_channel_volume(Sn76489 *p,unsigned c,unsigned v) { if(c<4) p->gain[c]=v; }
void psg_set_master_volume(Sn76489 *p,unsigned v) { p->master=v; }
void psg_update_timestamp(Sn76489 *p,int d) { p->time+=d; }
void psg_end_frame(Sn76489 *p,unsigned t) { sync_to(p,t); }
int psg_samples_avaliable(const Sn76489 *p) { return p->count-p->read; }
int psg_read_samples(Sn76489 *p,short *out,int n) {
    if(n>(int)(p->count-p->read)) n=p->count-p->read;
    for(int i=0;i<n;i++) { out[i*2]=p->samples[p->read*2]; out[i*2+1]=p->samples[p->read*2+1]; p->read++; }
    if(p->read==p->count) p->read=p->count=0;
    return n;
}
void psg_clear_samples(Sn76489 *p) { p->read=p->count=0; }
