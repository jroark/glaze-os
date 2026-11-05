/* 16550 FIFO semantics: THRE drops after the first byte, IIR reads ack THRE,
 * and a fast UART can empty between service and a trailing IIR read. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define GLAZE_HW_H
static uint32_t socket_config,irq_enable;
static uint8_t cor;
static bool present=true,has_fifo=true,instant;
static unsigned used,capacity=16,output_length,mmio,rx_at,rx_count;
static bool tx_irq;
static uint8_t regs[8],output[4096],input[64],lsr_error;
static uint8_t uart_read(unsigned);
static void uart_write(unsigned,uint8_t);
#define CF_UART_READ(r) uart_read(r)
#define CF_UART_WRITE(r,v) uart_write(r,v)
#define PCMCIA_CARD_PRESENT() present
#define PCMCIA_A020 socket_config
#define PCMCIA_A020_CONFIG 8u
#define KJ_SRC_PCMCIA irq_enable
#define KJ_SRC_ENABLE 8u
#define REG8(addr) cor
#define PCMCIA0_IO 0x0a00c000u
static uint32_t pcmcia_io_base = PCMCIA0_IO;
static uint32_t pcmcia_attr_base __attribute__((unused)) = 0x0a00d000u;  /* REG8 ignores the address */
#include "../../kernel/drivers/cfuart.c"
/* The verified BCSP setup (BLUETOOTH.md), as kernel/bluetooth.c passes it. */
static const struct uart_config bcsp = {
    .cor_address = 0xe0, .io_base = 0x3f8, .divisor = 1, .cor = 0x43, .lcr = 0x1b, .mcr = 0x0b, .fcr = 7,
};
void kprintf(const char *format,...){}
static uint8_t uart_read(unsigned r)
{
    mmio++;
    if(r==2){uint8_t value=(has_fifo && (regs[2]&1))?0xc0:0;
        value|=tx_irq && (regs[1]&2)?2:1;tx_irq=false;return value;}
    if(r==5)return (uint8_t)((used?0:0x60)|(rx_at<rx_count?1:0)|lsr_error);
    if(r==0){assert(rx_at<rx_count);return input[rx_at++];}
    return regs[r];
}
static void uart_write(unsigned r,uint8_t v)
{
    mmio++;
    if(r==0 && !(regs[3]&0x80)){
        assert(used<capacity && output_length<sizeof(output));used++;output[output_length++]=v;tx_irq=false;
        if(instant){used=0;tx_irq=true;}return;
    }
    if(r==1 && (regs[3]&0x80))return;
    regs[r]=v;
    if(r==1 && (v&2) && !used)tx_irq=true;
}
static void drained(void){used=0;if(regs[1]&2)tx_irq=true;}
static void initialize(bool fifo)
{
    present=true;has_fifo=fifo;capacity=fifo?16:1;used=output_length=rx_at=rx_count=0;tx_irq=false;instant=false;lsr_error=0;memset(regs,0,sizeof(regs));
    assert(!cfuart_init(&bcsp));assert(tx_fifo==capacity && cor==0x43 && regs[3]==0x1b && regs[1]==5);
}
int main(void)
{
    uint8_t data[40];for(unsigned i=0;i<sizeof(data);i++)data[i]=(uint8_t)i;
    initialize(true);assert(cfuart_write(data,sizeof(data))==sizeof(data));assert(output_length==16 && used==16 && regs[1]==7);
    cfuart_poll();assert(output_length==16);drained();cfuart_poll();assert(output_length==32);
    drained();cfuart_poll();assert(output_length==40 && regs[1]==5 && !memcmp(output,data,sizeof(data)));
    initialize(false);assert(cfuart_write(data,4)==4 && output_length==1);
    for(unsigned i=0;i<3;i++){drained();cfuart_poll();}assert(output_length==4 && !memcmp(output,data,4));
    initialize(true);instant=true;
    assert(cfuart_write(data,sizeof(data))==sizeof(data));assert(output_length==16 && tx_irq);
    cfuart_poll();assert(output_length==32 && tx_irq);cfuart_poll();assert(output_length==40 && regs[1]==5);
    /* Ring backpressure never overwrites queued bytes. */
    initialize(true);uint8_t large[TX_SIZE];memset(large,0xa5,sizeof(large));
    assert(cfuart_write(large,sizeof(large))==sizeof(large));assert(cfuart_write(data,17)==GZ_EBUSY);
    memcpy(input,data,sizeof(data));rx_count=sizeof(data);cfuart_poll();uint8_t received[40];assert(cfuart_read(received,40)==40 && !memcmp(received,data,40));cfuart_consume(40);
    present=false;unsigned before=mmio;cfuart_poll();cfuart_stop();assert(mmio==before);
    initialize(true);lsr_error=2;cfuart_poll();assert(cfuart_error()==GZ_EIO && !(irq_enable&8) && regs[1]==0);
    puts("Bluetooth UART: FIFO bursts, single-byte fallback, THRE acknowledgement race, ordering, backpressure, RX, removal and faults PASS");return 0;
}
