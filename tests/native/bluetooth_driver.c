/* Transport boundary: capabilities, callback validation, copy faults and swaps. */
#define GLAZE_HW_H
static int socket_present=1;
#define PCMCIA_CARD_PRESENT() (socket_present != 0)
#include "../../kernel/bluetooth.c"
#include <assert.h>
#include <stdio.h>
static unsigned consumed, sent, stops, unloaded;
static bool bad_callback, copy_fault;
static int load_error, init_error, uart_error;
static const struct uart_config *configured;
static int initialize(const struct uart_config *c) { configured = c; return init_error; }
static void poll_uart(void) {}
static void stop_uart(void) { stops++; }
static int error_uart(void) { return uart_error; }
static int read_uart(uint8_t *p,uint32_t n) { if(n>3)n=3; memset(p,0x42,n); return (int)n; }
static void consume_uart(uint32_t n) { consumed+=n; }
static int write_uart(const uint8_t *p,uint32_t n) { assert(p[0]==0x33); sent+=n; return (int)n; }
static void set_lines(uint8_t m) {}
static uint8_t msr(void) { return 0; }
static struct uart_driver descriptor={UART_DRIVER_MAGIC,UART_DRIVER_ABI,sizeof(struct uart_driver),
    initialize,poll_uart,stop_uart,error_uart,read_uart,consume_uart,write_uart,set_lines,msr};
int module_load(const char *path,const char *symbol,struct module *m)
{
    assert(!strcmp(path,"drivers/cfuart.ko") && !strcmp(symbol,"glaze_driver"));
    if(load_error)return load_error;
    m->entry=&descriptor; return 0;
}
void module_unload(struct module *m) { if(m->entry)unloaded++; memset(m,0,sizeof(*m)); }
bool module_contains(const struct module *m,const void *p,uint32_t size)
{ return p==&descriptor && size==sizeof(descriptor); }
bool module_executable(const struct module *m,const void *p) { return p && !bad_callback; }
void kputs(const char *s) {}
int copy_from_as(struct addrspace *as,void *p,uint32_t source,uint32_t size)
{ if(copy_fault)return GZ_EINVAL; memset(p,0x33,size); return 0; }
int copy_to_as(struct addrspace *as,uint32_t target,const void *p,uint32_t size)
{ assert(size==3 && ((const uint8_t *)p)[0]==0x42); return copy_fault ? GZ_EINVAL : 0; }
int main(void)
{
    struct proc service={.flags=PROC_BLUETOOTH},app={0};
    assert(sys_bt_io(&app,BT_IO_RESET,0,0)==GZ_EPERM);
    bt_insert(); assert(bt_present() && bt_ready()); service.bt_generation=bt_generation();
    /* The verified BCSP setup (BLUETOOTH.md): COR 43h at E0h, 3f8h, divisor 1, 8E1. */
    assert(configured->cor_address==0xe0 && configured->cor==0x43 && configured->io_base==0x3f8 &&
           configured->divisor==1 && configured->lcr==0x1b);
    assert(sys_bt_io(&service,BT_IO_READ,0,257)==GZ_EINVAL);
    copy_fault=true; assert(sys_bt_io(&service,BT_IO_READ,1,10)==GZ_EINVAL && consumed==0);
    assert(sys_bt_io(&service,BT_IO_WRITE,1,10)==GZ_EINVAL && sent==0);
    copy_fault=false; assert(sys_bt_io(&service,BT_IO_READ,1,10)==3 && consumed==3);
    assert(sys_bt_io(&service,BT_IO_WRITE,1,10)==10 && sent==10);
    uart_error=GZ_EIO; assert(bt_present() && !bt_ready());
    uart_error=0; assert(!sys_bt_io(&service,BT_IO_RESET,0,0));
    socket_present=0; assert(!bt_present() && sys_bt_io(&service,BT_IO_READ,0,3)==GZ_ENOENT);
    bt_removed(); assert(!bt_ready() && unloaded==1);
    assert(sys_bt_io(&service,BT_IO_READ,0,3)==GZ_EDEAD);
    socket_present=1; bad_callback=true; bt_insert(); assert(!bt_present() && unloaded==2); bt_removed();
    bad_callback=false; init_error=GZ_EIO; bt_insert(); assert(!bt_present() && unloaded==3); bt_removed();
    init_error=0; load_error=GZ_ENOENT; bt_insert(); assert(!bt_present()); bt_removed();
    load_error=0; bt_insert(); assert(bt_ready());
    assert(sys_bt_io(&service,BT_IO_READ,0,3)==GZ_EDEAD); bt_removed();
    puts("Bluetooth driver: privilege, stale generation, module validation, recovery, copy fault and removal PASS");
}
