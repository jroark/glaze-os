/* BCSP: 4-byte header, SLIP framing, reflected CCITT CRC with reversed
 * final bits, three-bit sequence/ACK and channel-1 link establishment.
 * Protocol reference: Linux drivers/bluetooth/hci_bcsp.c (v6.1).
 * A one-packet reliable window bounds storage and makes retransmits explicit.
 */
#include "bcsp.h"
#include <glaze/abi.h>
#include <string.h>
static const uint8_t sync_request[4] = {0xda,0xdc,0xed,0xed};
static const uint8_t sync_response[4] = {0xac,0xaf,0xef,0xee};
static const uint8_t conf_request[4] = {0xad,0xef,0xac,0xed};
static const uint8_t conf_response[4] = {0xde,0xad,0xd0,0xd0};
static bool due(uint32_t now, uint32_t end) { return (int32_t)(now-end) >= 0; }
static uint16_t crc_byte(uint16_t crc, uint8_t byte)
{
    crc ^= byte;
    for (unsigned bit=0; bit<8; bit++) crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
    return crc;
}
static uint16_t reverse(uint16_t value)
{
    uint16_t out=0;
    for (unsigned bit=0; bit<16; bit++) { out=(uint16_t)((out<<1)|(value&1)); value>>=1; }
    return out;
}
static void escaped(struct bcsp *b, uint8_t byte)
{
    if (byte==0xc0 || byte==0xdb) {
        b->wire[b->wire_length++]=0xdb;
        b->wire[b->wire_length++]=byte==0xc0 ? 0xdc : 0xdd;
    } else b->wire[b->wire_length++]=byte;
}
static bool frame(struct bcsp *b, unsigned channel, const uint8_t *data, unsigned count, bool reliable, unsigned seq)
{
    if (b->wire_length || count>sizeof(b->command)) return false;
    uint8_t header[4]; uint16_t crc=0xffff;
    header[0]=(uint8_t)(b->rxseq<<3 | (reliable ? 0xc0|seq : 0));
    header[1]=(uint8_t)((count<<4)|channel); header[2]=(uint8_t)(count>>4);
    header[3]=(uint8_t)~(header[0]+header[1]+header[2]);
    b->wire[b->wire_length++]=0xc0;
    for (unsigned i=0; i<4; i++) { crc=crc_byte(crc,header[i]); escaped(b,header[i]); }
    for (unsigned i=0; i<count; i++) { crc=crc_byte(crc,data[i]); escaped(b,data[i]); }
    if (reliable) { crc=reverse(crc); escaped(b,(uint8_t)(crc>>8)); escaped(b,(uint8_t)crc); }
    b->wire[b->wire_length++]=0xc0; b->wire_offset=0; b->ack=false;
    return true;
}
void bcsp_init(struct bcsp *b, int (*write)(void *, const uint8_t *, unsigned),
               void (*event)(void *, const uint8_t *, unsigned), void *arg, uint32_t now)
{
    memset(b,0,sizeof(*b)); b->write=write; b->event=event; b->arg=arg;
    b->began=now; b->link_deadline=now;
}
static int send_reliable(struct bcsp *b, unsigned channel, const uint8_t *command, unsigned count, uint32_t now)
{
    if (b->error) return b->error;
    if (b->state!=2) return GZ_ENOTCONN;
    if (!count || count>sizeof(b->command)) return GZ_EINVAL;
    if (b->pending || b->wire_length) return GZ_EBUSY;
    memcpy(b->command,command,count); b->command_length=count;
    b->command_seq=b->txseq; b->txseq=(b->txseq+1)&7;
    b->pending_channel=channel; b->pending=true; b->retries=0; b->deadline=now+250;
    frame(b,channel,command,count,true,b->command_seq); return 0;
}
int bcsp_command(struct bcsp *b,const uint8_t *p,unsigned n,uint32_t now)
{
    if(n<3 || n!=3u+p[2]) return GZ_EINVAL;
    return send_reliable(b,5,p,n,now);
}
int bcsp_acl(struct bcsp *b,const uint8_t *p,unsigned n,uint32_t now)
{
    if(n<4 || n!=4u+p[2]+((unsigned)p[3]<<8)) return GZ_EINVAL;
    return send_reliable(b,6,p,n,now);
}

static void receive(struct bcsp *b, uint32_t now)
{
    const uint8_t *p=b->input; unsigned n=b->input_length;
    if (n<4 || (uint8_t)(p[0]+p[1]+p[2]+p[3])!=255) return;
    unsigned count=(p[1]>>4)|(unsigned)p[2]<<4;
    bool crc_present=(p[0]&0x40)!=0;
    if (n!=4+count+(crc_present?2u:0u)) return;
    if (crc_present) {
        uint16_t crc=0xffff;
        for (unsigned i=0; i<4+count; i++) crc=crc_byte(crc,p[i]);
        crc=reverse(crc);
        if (p[4+count]!=(uint8_t)(crc>>8) || p[5+count]!=(uint8_t)crc) return;
    }
    unsigned channel=p[1]&15;
    if (channel==1 && !(p[0]&0x80) && count==4) {
        const uint8_t *data=p+4;
        if (!memcmp(data,sync_request,4)) {
            if (b->state==2) { b->error=GZ_ECONNRESET; return; }
            frame(b,1,sync_response,4,false,0);
        } else if (!memcmp(data,sync_response,4) && b->state==0) {
            b->state=1; b->link_deadline=now;
        } else if (!memcmp(data,conf_request,4)) {
            frame(b,1,conf_response,4,false,0);
        } else if (!memcmp(data,conf_response,4) && b->state==1) {
            b->state=2;
        }
        return;
    }
    if (b->state!=2) return;
    if (b->pending && ((p[0]>>3)&7)==((b->command_seq+1)&7)) b->pending=false;
    if (p[0]&0x80) {
        b->ack=true;
        if ((p[0]&7)!=b->rxseq) return; /* ACK duplicates; do not deliver twice */
        b->rxseq=(b->rxseq+1)&7;
        if (channel==5 && count>=2 && count==2u+p[5]) b->event(b->arg,p+4,count);
        if (channel==6 && b->acl && count>=4 && count==4u+p[6]+((unsigned)p[7]<<8)) b->acl(b->arg,p+4,count);
    }
}
void bcsp_feed(struct bcsp *b, const uint8_t *bytes, unsigned count, uint32_t now)
{
    for (unsigned i=0; i<count && !b->error; i++) {
        uint8_t ch=bytes[i];
        if (ch==0xc0) {
            if (b->started && !b->dropping && !b->escaped && b->input_length) receive(b,now);
            b->input_length=0; b->escaped=b->dropping=false; b->started=true;
            continue;
        }
        if (!b->started || b->dropping) continue;
        if (b->escaped) {
            b->escaped=false;
            if (ch==0xdc) ch=0xc0;
            else if (ch==0xdd) ch=0xdb;
            else { b->dropping=true; continue; }
        } else if (ch==0xdb) { b->escaped=true; continue; }
        if (b->input_length==sizeof(b->input)) { b->dropping=true; continue; }
        b->input[b->input_length++]=ch;
    }
}
void bcsp_tick(struct bcsp *b, uint32_t now)
{
    if (b->error) return;
    if (b->wire_length) {
        unsigned n=b->wire_length-b->wire_offset; if (n>256) n=256;
        int sent=b->write(b->arg,b->wire+b->wire_offset,n);
        if (sent<0 && sent!=GZ_EBUSY) { b->error=sent; return; }
        if (sent>0) {
            if ((unsigned)sent>n) { b->error=GZ_EIO; return; }
            b->wire_offset+=(unsigned)sent;
            if (b->wire_offset==b->wire_length) b->wire_length=b->wire_offset=0;
        }
        if (b->wire_length) return;
    }
    if (b->state!=2) {
        if (due(now,b->began+8000)) { b->error=GZ_ETIMEDOUT; return; }
        if (due(now,b->link_deadline)) {
            frame(b,1,b->state==0 ? sync_request : conf_request,4,false,0);
            b->link_deadline=now+250;
        }
    } else if (b->pending && due(now,b->deadline)) {
        if (++b->retries>12) { b->error=GZ_ETIMEDOUT; return; }
        frame(b,b->pending_channel,b->command,b->command_length,true,b->command_seq); b->deadline=now+250;
    } else if (b->ack) frame(b,0,NULL,0,false,0);
}
