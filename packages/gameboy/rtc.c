/* MBC3 clock: elapsed wall seconds, 512 days, independent of frame speed. */
#include "gameboy.h"
#include "../../third_party/gnuboy/defs.h"
#include "../../third_party/gnuboy/rtc.h"
#include <string.h>
struct rtc rtc;
static uint32_t last_wall;
static bool wall_valid;
void gb_clock_sync(uint32_t wall)
{
    if (!wall) return; /* RTC unavailable; retain last valid reference. */
    uint32_t elapsed=wall_valid && wall>last_wall ? wall-last_wall : 0;
    last_wall=wall; wall_valid=true;
    if (!rtc.batt || rtc.stop) return;
    uint64_t total=(uint64_t)rtc.d*86400u+rtc.h*3600u+rtc.m*60u+rtc.s+elapsed;
    uint64_t days=total/86400u;
    if (days>=512) rtc.carry=1;
    rtc.d=days&511;
    uint32_t part=total%86400u;
    rtc.h=part/3600u; rtc.m=part/60u%60; rtc.s=part%60;
}
void rtc_latch(byte b)
{
    if (rtc.latch==0 && b==1) {
        rtc.regs[0]=rtc.s; rtc.regs[1]=rtc.m; rtc.regs[2]=rtc.h;
        rtc.regs[3]=rtc.d; rtc.regs[4]=(rtc.d>>8)|(rtc.stop<<6)|(rtc.carry<<7);
    }
    rtc.latch=b;
}
void rtc_write(byte b)
{
    switch(rtc.sel) {
    case 8: rtc.s=(b&63)%60; break;
    case 9: rtc.m=(b&63)%60; break;
    case 10: rtc.h=(b&31)%24; break;
    case 11: rtc.d=(rtc.d&256)|b; break;
    case 12: rtc.d=(rtc.d&255)|((b&1)<<8); rtc.stop=!!(b&64); rtc.carry=!!(b&128); break;
    }
}
void gb_clock_export(uint8_t out[GB_RTC_SIZE],uint32_t wall)
{
    gb_clock_sync(wall);
    memset(out,0,GB_RTC_SIZE);
    memcpy(out,"GRTC",4); out[4]=1;
    out[5]=rtc.s; out[6]=rtc.m; out[7]=rtc.h;
    out[8]=rtc.d; out[9]=rtc.d>>8; out[10]=rtc.stop; out[11]=rtc.carry;
    for(unsigned i=0;i<4;i++) out[12+i]=last_wall>>(i*8);
    memcpy(out+16,rtc.regs,5); out[21]=rtc.latch;
    uint32_t hash=gb_hash(out,28);
    for(unsigned i=0;i<4;i++) out[28+i]=hash>>(i*8);
}
bool gb_clock_import(const uint8_t in[GB_RTC_SIZE],uint32_t wall)
{
    uint32_t hash=0,stamp=0;
    for(unsigned i=0;i<4;i++) { hash|=(uint32_t)in[28+i]<<(i*8); stamp|=(uint32_t)in[12+i]<<(i*8); }
    if (memcmp(in,"GRTC",4) || in[4]!=1 || in[5]>=60 || in[6]>=60 ||
        in[7]>=24 || in[9]>1 || in[10]>1 || in[11]>1 || hash!=gb_hash(in,28)) return false;
    rtc.s=in[5]; rtc.m=in[6]; rtc.h=in[7]; rtc.d=in[8]|((int)in[9]<<8);
    rtc.stop=in[10]; rtc.carry=in[11]; memcpy(rtc.regs,in+16,5); rtc.latch=in[21];
    last_wall=stamp; wall_valid=stamp!=0; gb_clock_sync(wall);
    return true;
}
void gb_clock_init(uint32_t wall) { last_wall=wall; wall_valid=wall!=0; }
