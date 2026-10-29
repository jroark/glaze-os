/* Real host pump + production device parser on fragmented USB byte streams.
 * Network fds are datagram pairs: exercise TAP and Darwin's utun packet ABI
 * without needing root, a kernel extension or changes to the host routes. */
#include "../host/glaze_usb/client.h"
#include "../host/glaze_usb/ethernet_packet.h"
#include "../servers/usb/ethernet.h"
#include <glaze/usb.h>
#include <glaze/usb_net.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

volatile sig_atomic_t usb_cancelled;
static uint8_t input[GU_FRAME], output[GU_FRAME], sent_frame[GUN_FRAME_MAX];
static unsigned input_len,input_pos,output_len,exchanges,opened,closed,accepted_frames;
static uint32_t clock_ms;
static int link_up, mode, end_after;
static const uint8_t device_mac[6]=GUN_DEVICE_MAC, host_mac[6]=GUN_HOST_MAC;
static uint8_t ip[4+40], arp_request[60];
uint32_t gz_time_ms(void){return clock_ms;}
void gz_sleep_ms(uint32_t ms){clock_ms+=ms;}
int gz_usb_ctl(uint32_t op,void *buffer,uint32_t size)
{
    if(op==GZ_USB_NET_OPEN){assert(!size);link_up=1;opened++;return 0;}
    if(op==GZ_USB_NET_CLOSE){assert(!size);link_up=0;closed++;return 0;}
    if(op==GZ_USB_READ){unsigned n=input_len-input_pos;if(n>size)n=size;if(n>17)n=17;memcpy(buffer,input+input_pos,n);input_pos+=n;return n;}
    if(op==GZ_USB_WRITE){if(size>7)size=7;assert(output_len+size<=sizeof(output));memcpy(output+output_len,buffer,size);output_len+=size;return size;}
    assert(op==GZ_USB_NET_EXCHANGE && link_up);
    exchanges++;
    if(size){assert(size>=14 && size<=GUN_FRAME_MAX);memcpy(sent_frame,buffer,size);}
    /* First input meets backpressure while device output still progresses. */
    unsigned accepted=size && exchanges!=1;
    if(accepted)accepted_frames++;
    gu_put32(buffer,accepted);
    if(exchanges==1 && mode){memcpy((uint8_t *)buffer+4,arp_request,60);return 64;}
    if(exchanges==2){
        assert(size && accepted);
        if(mode){
            assert(sent_frame[12]==8 && sent_frame[13]==0 && !memcmp(sent_frame+14,ip+4,40));
            unsigned n=gu_eth_from_ip((uint8_t *)buffer+4,ip,sizeof(ip));
            return n+4;
        }
        assert(size==60 && sent_frame[59]==0x73);
        memset((uint8_t *)buffer+4,0x4c,60);return 64;
    }
    if(mode && exchanges==3){assert(size==60 && sent_frame[21]==2);assert(!memcmp(sent_frame,device_mac,6));}
    if(end_after && exchanges>=5)usb_cancelled=1;
    return 4;
}
static int transact(uint32_t op,uint32_t id,const void *data,unsigned len,int corrupt,uint8_t *reply)
{
    input_len=gu_encode(input,op,id,0,data,len);input_pos=output_len=0;
    if(corrupt)input[20]^=1;
    for(unsigned i=0;i<1000 && input_pos<input_len;i++)usb_ethernet_poll(3);
    if(corrupt){assert(!output_len);return 0;}
    assert(output_len>=GU_HEADER);
    struct gu_parser parser={0};
    for(unsigned i=0;i<output_len;i++)assert(gu_feed(&parser,output[i])==(i+1==output_len?1:0));
    assert(gu_get32(output+8)==id && (gu_get32(output+4)>>16)==(op|GU_REPLY));
    if(reply)memcpy(reply,output,output_len);
    return (int32_t)gu_get32(output+16);
}
int gu_call(struct gu_client *c,uint32_t op,const void *data,uint32_t len)
{
    unsigned id=++c->id;
    if(op==GU_USB_NET_OPEN){
        gu_encode(input,op,id,0,NULL,0);
        return usb_ethernet_open(id,gu_get32(input+20));
    }
    int status=transact(op,id,data,len,0,c->frame);
    return status ? -1 : (int)gu_get32(c->frame+12);
}
static void packets(void)
{
    memset(ip,0,sizeof(ip));ip[3]=2;ip[4]=0x45;ip[6]=0;ip[7]=40;
    uint8_t ethernet[GUN_FRAME_MAX],roundtrip[GUN_MTU+4],arp[60];unsigned n;
    assert(gu_eth_from_ip(ethernet,ip,sizeof(ip))==54);
    assert(!memcmp(ethernet,device_mac,6) && !memcmp(ethernet+6,host_mac,6));
    memset(ethernet+54,0,6);
    assert(gu_eth_to_ip(roundtrip,ethernet,60,arp,&n)==sizeof(ip) && !memcmp(roundtrip,ip,sizeof(ip)) && !n);
    ip[3]=30;assert(!gu_eth_from_ip(ethernet,ip,sizeof(ip)));ip[3]=2;
    ip[7]=41;assert(!gu_eth_from_ip(ethernet,ip,sizeof(ip)));ip[7]=40;
    ip[4]=0x44;assert(!gu_eth_from_ip(ethernet,ip,sizeof(ip)));ip[4]=0x45;
    for(unsigned size=0;size<44;size++)assert(!gu_eth_from_ip(ethernet,ip,size));
    memset(arp_request,0,60);memset(arp_request,255,6);memcpy(arp_request+6,device_mac,6);
    arp_request[12]=8;arp_request[13]=6;
    uint8_t header[]={0,1,8,0,6,4,0,1};memcpy(arp_request+14,header,8);
    memcpy(arp_request+22,device_mac,6);memcpy(arp_request+28,(uint8_t[]){192,168,7,2},4);
    memcpy(arp_request+38,(uint8_t[]){192,168,7,1},4);
    assert(!gu_eth_to_ip(roundtrip,arp_request,60,arp,&n) && n==60 && arp[21]==2);
    assert(!memcmp(arp+22,host_mac,6) && !memcmp(arp+32,device_mac,6));
    arp_request[41]=3;assert(!gu_eth_to_ip(roundtrip,arp_request,60,arp,&n) && !n);arp_request[41]=1;
    for(unsigned size=0;size<42;size++)assert(!gu_eth_to_ip(roundtrip,arp_request,size,arp,&n) && !n);
}
int main(void)
{
    packets();
    uint8_t body[60]={0},first[GU_FRAME],replay[GU_FRAME];
    gu_encode(input,GU_USB_NET_OPEN,1,0,NULL,0);
    assert(!usb_ethernet_open(1,gu_get32(input+20)));usb_ethernet_poll(3);
    assert(!transact(GU_USB_NET_OPEN,1,NULL,0,0,replay) && opened==1); /* OPEN replay cannot reopen/reset queues */
    assert(transact(GU_HELLO,1,NULL,0,0,replay)==GZ_EINVAL);
    assert(!transact(GU_USB_NET_EXCHANGE,2,body,60,0,first));assert(exchanges==1);
    assert(!transact(GU_USB_NET_EXCHANGE,2,body,60,0,replay));
    assert(exchanges==1 && !memcmp(first,replay,output_len));
    assert(transact(GU_HELLO,3,NULL,0,0,NULL)==GZ_EINVAL); /* no debugger in the tunnel */
    transact(GU_USB_NET_EXCHANGE,4,body,60,1,NULL);assert(exchanges==1);
    assert(!transact(GU_USB_NET_CLOSE,4,NULL,0,0,NULL) && !usb_ethernet_active() && !link_up);
    assert(!usb_ethernet_open(1,123));usb_ethernet_poll(3);usb_ethernet_poll(7);
    assert(!usb_ethernet_active() && !link_up);
    assert(!usb_ethernet_open(1,123));usb_ethernet_poll(3);clock_ms+=10000;usb_ethernet_poll(3);
    assert(!usb_ethernet_active() && !link_up);
    for(mode=0;mode<=1;mode++) {
        int pair[2];assert(!socketpair(AF_UNIX,SOCK_DGRAM,0,pair));
        assert(fcntl(pair[0],F_SETFL,O_NONBLOCK)==0);assert(fcntl(pair[1],F_SETFL,O_NONBLOCK)==0);
        memset(body,0x31,60);body[59]=0x73;
        assert(write(pair[1],mode?ip:body,mode?sizeof(ip):60)==(ssize_t)(mode?sizeof(ip):60));
        struct usb_link usb={0};struct gu_client c={.usb=&usb,.caps=GU_CAP_USB_NET};
        exchanges=accepted_frames=0;usb_cancelled=0;end_after=1;
        assert(!gu_usb_net_pump(&c,pair[0],mode));
        uint8_t got[1600];int n=read(pair[1],got,sizeof(got));
        if(mode)assert(n==sizeof(ip) && !memcmp(got,ip,sizeof(ip)) && accepted_frames==2);
        else assert(n==60 && got[59]==0x4c && accepted_frames==1);
        /* Actual CLI close drops DTR on cancellation; simulate that event. */
        usb_ethernet_poll(0);assert(!usb_ethernet_active());close(pair[0]);close(pair[1]);
    }
    assert(opened==closed);
    puts("usb-ethernet: TAP/utun framing, ARP, backpressure, replay, fragmented USB, disconnect and timeout PASS");
    return 0;
}
