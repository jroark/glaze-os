/* A single event loop keeps discovery asynchronous and bounds UART work. */
#include "controller.h"
#include "a2dp.h"
#include <glaze/sys.h>
#include <stdio.h>
#include <stddef.h>
_Static_assert(offsetof(struct bt_status,devices)<=60,"BT status IPC size");
_Static_assert(sizeof(struct bt_device)<=60,"BT device IPC size");
#include <string.h>
static struct bt_controller controller;
static struct bt_a2dp audio;
static void *pcm_buffer;
static int pcm_pid;
static uint32_t pcm_token,next_pcm=1;
static void detach_pcm(void)
{
    if(pcm_buffer)gz_shm_unmap(pcm_buffer);
    pcm_buffer=NULL;pcm_pid=0;pcm_token=0;
    bt_a2dp_flush(&audio);
}
_Static_assert(sizeof(struct bt_audio_status)<=60,"audio IPC size");
#ifdef BT_DEBUG
static unsigned traces;
static void trace(const char *direction,const uint8_t *p,unsigned n)
{
    if (traces++>=50) return;
    printf("bt-debug: %s %u:",direction,n);
    for (unsigned i=0;i<n && i<64;i++) printf(" %02x",p[i]);
    printf("\n");
}
#endif
static int uart_write(void *arg,const uint8_t *p,unsigned n)
{
    (void)arg;
#ifdef BT_DEBUG
    trace("TX",p,n);
#endif
    return gz_bt_io(BT_IO_WRITE,(void *)p,n);
}
static void status_reply(struct gz_msg *r)
{
    /* Fixed header fits an IPC reply; devices are fetched by revision/index. */
    memcpy(&r->w[1],&controller.status,offsetof(struct bt_status,devices));
}
int main(void)
{
    struct bt_card_info card;
    if (gz_bt_card_info(&card,sizeof(card)) || card.kind!=BT_CARD_BLUETOOTH || gz_bt_io(BT_IO_RESET,NULL,0)) return 1;
    int port=gz_port_create();
    if (port<0 || gz_port_register(port,BT_PORT_NAME)) return 1;
    bt_controller_init(&controller,uart_write,NULL,card.generation,gz_time_ms());
    bt_a2dp_init(&audio,&controller.serial);
    uint32_t audio_reported=~0u;
    uint32_t reported=~0u, serial_reported=~0u, next_serial=1, retry_at=0;
    for (;;) {
        uint32_t now=gz_time_ms();
        uint8_t bytes[256];
        /* At most 1 KiB per pass, followed by IPC even during noisy input. */
        for (unsigned i=0; i<4; i++) {
            int n=gz_bt_io(BT_IO_READ,bytes,sizeof(bytes));
            if (n<0) { controller.link.error=n; break; }
            if (!n) break;
#ifdef BT_DEBUG
            trace("RX",bytes,(unsigned)n);
#endif
            bt_controller_feed(&controller,bytes,(unsigned)n,now);
        }
        bt_controller_tick(&controller,now);
        if(controller.status.state==BT_FAILED && controller.serial.profile==&audio)bt_l2_fail(&controller.serial,controller.status.error);
        bt_a2dp_tick(&audio,now);
        if(pcm_buffer && (audio.status.state!=BT_AUDIO_STREAMING || !gz_proc_alive(pcm_pid)))detach_pcm();
        if(audio_reported!=audio.status.state){
            audio_reported=audio.status.state;
#ifdef BT_SERIAL_DEBUG
            if(audio_reported==BT_AUDIO_FAILED)printf("bluetooth audio: transport q %u offset %u credit %u pending %u cids %x/%x/%x handle %x\n",controller.serial.qh-controller.serial.qt,controller.serial.offset,controller.acl_credit,controller.link.pending,controller.serial.channels[0].remote,controller.serial.channels[1].remote,controller.serial.channels[2].remote,controller.serial.handle);
#endif
            printf("bluetooth audio: state %u error %d phase %u endpoint %u rate %u channels %u bitpool %u\n",audio_reported,audio.status.error,audio.phase,audio.seid,audio.status.rate,audio.status.channels,audio.status.bitpool);
        }
        if (reported!=controller.status.state) {
            reported=controller.status.state;
            printf("bluetooth: state %u error %d devices %u link %u phase %u opcode %04x\n",reported,controller.status.error,controller.status.count,controller.link.state,controller.phase,controller.opcode);
            if (reported==BT_READY) printf("bluetooth: address %02x:%02x:%02x:%02x:%02x:%02x HCI %u manufacturer %u\n",
                controller.status.address[5],controller.status.address[4],controller.status.address[3],
                controller.status.address[2],controller.status.address[1],controller.status.address[0],
                controller.status.hci_version,controller.status.manufacturer);
            if (reported==BT_FAILED) retry_at=now+5000;
        }
        if(serial_reported!=controller.serial.status.state) {
            serial_reported=controller.serial.status.state;
            printf("bluetooth serial: state %u error %d stage %u channel %u\n",serial_reported,controller.serial.status.error,controller.serial.stage,controller.serial.status.channel);
        }
        if (controller.status.state==BT_FAILED && (int32_t)(now-retry_at)>=0) {
            if (gz_bt_io(BT_IO_RESET,NULL,0)) return 1;
            uint32_t revision=controller.status.revision+1;
            detach_pcm();
            bt_controller_init(&controller,uart_write,NULL,card.generation,now);
            bt_a2dp_init(&audio,&controller.serial);
            controller.status.revision=revision;
        }
        struct gz_msg m={{0}},r={{0}};
        int token=gz_recv(port,&m,5);
        if (token<=0) continue;
        switch (m.w[0]) {
        case BT_AUDIO_CONNECT:
        case BT_SERIAL_CONNECT: {
#ifdef BT_DEBUG
            traces=50; /* Pairing traffic must never enter packet logs. */
#endif
            if(controller.status.state!=BT_READY)r.w[0]=(uint32_t)GZ_EBUSY;
            else if(!next_serial)r.w[0]=(uint32_t)GZ_EFULL;
            else {
                const uint8_t *bytes=(const uint8_t *)&m.w[1];
                int error=m.w[0]==BT_AUDIO_CONNECT?bt_a2dp_start(&audio,bytes,bytes+7,bytes[6],next_serial,gz_time_ms()):bytes[23]>30?GZ_EINVAL:bt_serial_start(&controller.serial,bytes,bytes+7,bytes[6],next_serial,gz_time_ms());
                r.w[0]=(uint32_t)error;
                if(!error){if(m.w[0]==BT_SERIAL_CONNECT){controller.serial.status.channel=bytes[23];audio.status=(struct bt_audio_status){0};}r.w[1]=next_serial++;}
            }
            break;
        }
        case BT_SERIAL_STATUS:
            if(!controller.serial.profile)memcpy(&r.w[1],&controller.serial.status,sizeof(struct bt_serial_status));
            break;
        case BT_AUDIO_STATUS: memcpy(&r.w[1],&audio.status,sizeof(audio.status));break;
        case BT_AUDIO_CLOSE:
            if(m.w[1]!=audio.status.id || controller.serial.profile!=&audio)r.w[0]=(uint32_t)GZ_EDEAD;
            else {detach_pcm();r.w[0]=(uint32_t)bt_serial_close(&controller.serial);}break;
        case BT_AUDIO_TONE:
            if(m.w[1]!=audio.status.id)r.w[0]=(uint32_t)GZ_EDEAD;
            else if(pcm_buffer)r.w[0]=(uint32_t)GZ_EBUSY;
            else r.w[0]=(uint32_t)bt_a2dp_tone(&audio,m.w[2]);
            break;
        case BT_AUDIO_ATTACH:
            if(m.w[1]!=audio.status.id)r.w[0]=(uint32_t)GZ_EDEAD;
            else if(audio.status.state!=BT_AUDIO_STREAMING)r.w[0]=(uint32_t)GZ_ENOTCONN;
            else if(pcm_buffer || audio.tone_left)r.w[0]=(uint32_t)GZ_EBUSY;
            else if(!next_pcm)r.w[0]=(uint32_t)GZ_EFULL;
            else {
                int shm=gz_shm_create(BT_AUDIO_BUF_SIZE);
                if(shm<0)r.w[0]=(uint32_t)shm;
                else if(!(pcm_buffer=gz_shm_map(shm)))r.w[0]=(uint32_t)GZ_ENOMEM;
                else {pcm_pid=(int)m.w[2];pcm_token=next_pcm++;r.w[1]=(uint32_t)shm;r.w[2]=pcm_token;}
            }break;
        case BT_AUDIO_PLAY:
        case BT_AUDIO_PAUSE:
        case BT_AUDIO_WRITE:
        case BT_AUDIO_FLUSH:
        case BT_AUDIO_DETACH:
            if(m.w[1]!=audio.status.id || !pcm_buffer || m.w[2]!=pcm_token)r.w[0]=(uint32_t)GZ_EDEAD;
            else if(m.w[0]==BT_AUDIO_PLAY)audio.playing=true;
            else if(m.w[0]==BT_AUDIO_PAUSE)audio.playing=false;
            else if(m.w[0]==BT_AUDIO_DETACH)detach_pcm();
            else if(m.w[0]==BT_AUDIO_FLUSH)bt_a2dp_flush(&audio);
            else if(m.w[3]>BT_AUDIO_BUF_SIZE)r.w[0]=(uint32_t)GZ_EINVAL;
            else r.w[0]=(uint32_t)bt_a2dp_write(&audio,pcm_buffer,m.w[3]);
            break;
        case BT_SERIAL_CLOSE:
        case BT_SERIAL_WRITE:
        case BT_SERIAL_READ:
            if(controller.serial.profile || m.w[1]!=controller.serial.status.id)r.w[0]=(uint32_t)GZ_EDEAD;
            else if(m.w[0]==BT_SERIAL_CLOSE)r.w[0]=(uint32_t)bt_serial_close(&controller.serial);
            else if(m.w[2]>48)r.w[0]=(uint32_t)GZ_EINVAL;
            else if(m.w[0]==BT_SERIAL_WRITE)r.w[0]=(uint32_t)bt_serial_tx(&controller.serial,(const uint8_t *)&m.w[3],m.w[2]);
            else r.w[0]=(uint32_t)bt_serial_rx(&controller.serial,(uint8_t *)&r.w[1],m.w[2]);
            break;
        case BT_STATUS: status_reply(&r); break;
        case BT_SCAN: r.w[0]=(uint32_t)bt_controller_scan(&controller); break;
        case BT_CANCEL: r.w[0]=(uint32_t)bt_controller_cancel(&controller); break;
        case BT_DEVICE:
            if (m.w[3]!=controller.status.generation) r.w[0]=(uint32_t)GZ_EDEAD;
            else if (m.w[1]!=controller.status.revision) r.w[0]=(uint32_t)GZ_EBUSY;
            else if (m.w[2]>=controller.status.count) r.w[0]=(uint32_t)GZ_EINVAL;
            else memcpy(&r.w[1],&controller.status.devices[m.w[2]],sizeof(struct bt_device));
            break;
        default: r.w[0]=(uint32_t)GZ_EINVAL; break;
        }
        gz_reply(token,&r);
    }
}
