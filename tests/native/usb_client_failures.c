/* Production framing: transport failures are distinguishable from rejection,
 * and a mutating call is sent once even when its reply never arrives. */
#include <time.h>
#include <assert.h>
#include <string.h>
static unsigned ticks, writes, mode, request_op, request_id;
static int fake_clock_gettime(clockid_t id,struct timespec *t)
{ (void)id; t->tv_sec=++ticks; t->tv_nsec=0; return 0; }
#define clock_gettime fake_clock_gettime
#include "../../tools/glaze-usb/client.c"
#undef clock_gettime
volatile sig_atomic_t usb_cancelled;
int usb_lines(struct usb_link *l,int value) { (void)l;(void)value;return 0; }
int usb_write(struct usb_link *l,const void *p,size_t n,unsigned timeout)
{
    (void)l;(void)n;(void)timeout;writes++;
    request_op=gu_get32((const uint8_t *)p+4)>>16;
    request_id=gu_get32((const uint8_t *)p+8);
    return mode==1 ? LIBUSB_ERROR_IO : 0;
}
int usb_read(struct usb_link *l,void *p,size_t n,unsigned timeout)
{
    (void)l;(void)n;(void)timeout;
    if(mode==2)return LIBUSB_ERROR_TIMEOUT;
    uint8_t data[44]={0};gu_put32(data,GU_VERSION);
    memcpy(data+12,"fixture-build",13);
    return (int)gu_encode(p,request_op|GU_REPLY,request_id,mode==3 ? -5 : 0,
                          data,request_op==GU_HELLO ? sizeof(data) : 4);
}
int main(void)
{
    struct usb_link l={0};struct gu_client c={.usb=&l};
    mode=1;assert(gu_call(&c,GU_NAND_READ,NULL,0)<0 && c.transport_failed && writes==1);
    mode=2;writes=0;assert(gu_call(&c,GU_NAND_REPAIR,NULL,0)<0 && c.transport_failed && writes==1);
    mode=3;writes=0;assert(gu_call(&c,GU_NAND_READ,NULL,0)<0 && !c.transport_failed && writes==1);
    mode=0;assert(!gu_hello(&c,&l) && !strcmp(c.build,"fixture-build") && !c.recover_read);
    puts("usb-client: transport classification, single-send mutation, verified build PASS");
}
