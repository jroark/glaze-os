/* A single event loop keeps discovery asynchronous and bounds UART work. */
#include "controller.h"
#include <glaze/sys.h>
#include <stdio.h>
#include <stddef.h>
_Static_assert(offsetof(struct bt_status,devices)<=60,"BT status IPC size");
_Static_assert(sizeof(struct bt_device)<=60,"BT device IPC size");
#include <string.h>
static struct bt_controller controller;
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
            bt_controller_init(&controller,uart_write,NULL,card.generation,now);
            controller.status.revision=revision;
        }
        struct gz_msg m={{0}},r={{0}};
        int token=gz_recv(port,&m,5);
        if (token<=0) continue;
        switch (m.w[0]) {
        case BT_SERIAL_CONNECT: {
#ifdef BT_DEBUG
            traces=50; /* Pairing traffic must never enter packet logs. */
#endif
            if(controller.status.state!=BT_READY)r.w[0]=(uint32_t)GZ_EBUSY;
            else if(!next_serial)r.w[0]=(uint32_t)GZ_EFULL;
            else {
                const uint8_t *bytes=(const uint8_t *)&m.w[1];
                int error=bytes[23]>30?GZ_EINVAL:bt_serial_start(&controller.serial,bytes,bytes+7,bytes[6],next_serial,gz_time_ms());
                r.w[0]=(uint32_t)error;
                if(!error){controller.serial.status.channel=bytes[23];r.w[1]=next_serial++;}
            }
            break;
        }
        case BT_SERIAL_STATUS: memcpy(&r.w[1],&controller.serial.status,sizeof(struct bt_serial_status));break;
        case BT_SERIAL_CLOSE:
        case BT_SERIAL_WRITE:
        case BT_SERIAL_READ:
            if(m.w[1]!=controller.serial.status.id)r.w[0]=(uint32_t)GZ_EDEAD;
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
