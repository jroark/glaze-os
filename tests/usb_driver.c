/* Production driver against the emulator's independent USBF hardware model. */
#include "../kernel/drivers.h"
#include "../kernel/kernel.h"
#include <glaze/usb.h>
#include <assert.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#include "usbf.h"
static usbf_state_t model;
static uint8_t ram[65536], user[GU_SCREEN_BYTES + 16];
static uint16_t framebuffer[GZ_FB_STRIDE * GZ_FB_HEIGHT];
#define USB_FRAMEBUFFER framebuffer
static uint32_t gate;
static jmp_buf reboot_return;
static unsigned rebooted;
#define USB_READ(off) (usbf_handles(off) ? usbf_read(&model, off) : 0)
#define USB_WRITE(off, v) do { if (usbf_handles(off)) usbf_write(&model, off, v); } while (0)
#define KJ_GIRQ0_EN(n) gate
#define KJ_SRC_ENABLE 8
#define KSEG1(pa) (ram + (pa))
#define PHYS(p) ((uintptr_t)(p) - (uintptr_t)ram)
#define USB_TEST
#include "../kernel/drivers/usb.c"
void *page_alloc(void) { return ram + 4096; }
void mm_sync_code(void *p, uint32_t n) { (void)p; (void)n; }
int copy_to_as(struct addrspace *as, uint32_t uva, const void *p, uint32_t n)
{ (void)as; if (uva > sizeof(user) || n > sizeof(user) - uva) return -1; memcpy(user + uva, p, n); return 0; }
int copy_from_as(struct addrspace *as, void *p, uint32_t uva, uint32_t n)
{ (void)as; if (uva > sizeof(user) || n > sizeof(user) - uva) return -1; memcpy(p, user + uva, n); return 0; }
int usb_reload_ctl(struct proc *p, uint32_t op, uint32_t b, uint32_t n)
{ (void)p; (void)op; (void)b; (void)n; return GZ_EINVAL; }
static unsigned input_cancels;
static unsigned net_stops, net_calls;
void usbnet_stop(void) { net_stops++; }
int usbnet_ctl(struct proc *p, uint32_t op, uint32_t b, uint32_t n)
{ net_calls++; return 456; }
void usb_input_cancel(void) { input_cancels++; }
void machine_reboot_ce(void) { rebooted++; longjmp(reboot_return, 1); }
uint64_t time_us(void) { static uint64_t now; now += 100000; return now; }
int usb_input_ctl(struct proc *p, uint32_t op, uint32_t b, uint32_t n)
{ (void)p; (void)op; (void)b; (void)n; return 123; }
static uint8_t *model_ram(void *ctx, uint32_t pa, uint32_t n)
{ (void)ctx; assert(pa <= sizeof(ram) && n <= sizeof(ram) - pa); return ram + pa; }
static void set_lines(unsigned value)
{
    uint8_t req[8] = {0x21,0x22,(uint8_t)value}, reply[8]; uint16_t n = sizeof(reply);
    assert(usbf_host_control(&model, req, reply, &n) == USBF_CTRL_PENDING);
    for (int i = 0; i < 4; i++) usb_poll();
    assert(!model.ctrl_pending);
}
int main(void)
{
    model.ram = model_ram; usbf_init(&model); usb_init();
    assert(active && usbf_descriptors_ready(&model));
    usbf_host_attach(&model, true);
    usbf_host_bus_reset(&model); usb_poll();
    uint8_t setup[8] = {0,9,1}, answer[64]; uint16_t len = sizeof(answer);
    assert(usbf_host_control(&model, setup, answer, &len) == USBF_CTRL_OK);
    usb_poll(); assert(configured); set_lines(3);
    /* Control/status diagnostics must work without using bulk DMA. */
    assert(usbf_host_intr_in(&model, answer, sizeof(answer)) == 2);
    uint8_t diagnostic_req[8] = {0x40,0x5a};
    len = sizeof(answer);
    assert(usbf_host_control(&model, diagnostic_req, answer, &len) == USBF_CTRL_PENDING);
    for (int i = 0; i < 4; i++) usb_poll();
    assert(usbf_host_intr_in(&model, answer, sizeof(answer)) == 8);
    assert(answer[0] == 'G' && answer[1] == 'Z');
    struct proc p = { .flags = PROC_USB }, denied = {0};
    assert(sys_usb_ctl(&denied, GZ_USB_STATE, 0, 0) == GZ_EPERM);
    assert(sys_usb_ctl(&denied, GZ_USB_NET_OPEN, 0, 0) == GZ_EPERM && !net_calls);
    assert(sys_usb_ctl(&p, GZ_USB_NET_OPEN, 0, 0) == 456 && net_calls == 1);
    assert(sys_usb_ctl(&denied, GZ_USB_REBOOT_CE, 0, 0) == GZ_EPERM);
    assert(sys_usb_ctl(&p, GZ_USB_REBOOT_CE, 0, 1) == GZ_EINVAL && !rebooted);
    assert(sys_usb_ctl(&denied, GZ_USB_SCREEN_COPY, 0, GU_SCREEN_BYTES) == GZ_EPERM);
    for (unsigned y=0;y<GZ_FB_HEIGHT;y++) for(unsigned x=0;x<GZ_FB_STRIDE;x++)
        framebuffer[y*GZ_FB_STRIDE+x]=(uint16_t)(x ^ y<<8);
    assert(sys_usb_ctl(&p,GZ_USB_SCREEN_COPY,0,GU_SCREEN_BYTES-1)==GZ_EINVAL);
    assert(sys_usb_ctl(&p,GZ_USB_SCREEN_COPY,0,GU_SCREEN_BYTES)==0);
    for (unsigned y=0;y<GZ_FB_HEIGHT;y++) for(unsigned x=0;x<GZ_FB_WIDTH;x++) {
        unsigned at=(y*GZ_FB_WIDTH+x)*2;
        assert((user[at]|(unsigned)user[at+1]<<8)==framebuffer[y*GZ_FB_STRIDE+x]);
    }
    assert(sys_usb_ctl(&p,GZ_USB_SCREEN_COPY,32,GU_SCREEN_BYTES)==GZ_EINVAL);
    assert(sys_usb_ctl(&p,GZ_USB_SCRIPT_STATUS,0,16)==123);
    assert(sys_usb_ctl(&p, GZ_USB_READ, sizeof(user), 1) == 0);
    for (unsigned size = 1; size <= 128; size++) {
        for (unsigned i = 0; i < size; i++) user[i] = (uint8_t)i;
        assert(sys_usb_ctl(&p, GZ_USB_WRITE, 0, size) == (int)size);
        uint8_t got[256]; unsigned count = 0;
        for (unsigned tick = 0; tick < 20; tick++) {
            usb_poll(); int n = usbf_host_in(&model, answer, sizeof(answer));
            if (n >= 0) { assert(count + (unsigned)n <= sizeof(got)); memcpy(got + count, answer, (size_t)n); count += (unsigned)n; }
            if (usb_tx_idle()) break;
        }
        assert(count == size && !memcmp(user, got, size));
    }
    for (unsigned i = 0; i < 64; i++) answer[i] = (uint8_t)(i ^ 0x85);
    assert(usbf_host_out(&model, answer, 64)); usb_poll();
    assert(sys_usb_ctl(&p, GZ_USB_READ, sizeof(user) - 2, 64) == GZ_EINVAL);
    assert(sys_usb_ctl(&p, GZ_USB_READ, 0, 64) == 64 && !memcmp(user, answer, 64));
    for (unsigned i = 0; i < RING + 7; i++) usb_log_putc('x');
    assert(sys_usb_ctl(&p, GZ_USB_LOG_READ, 0, 256) == 256 && gu_get32(user) == 7);
    /* A reset must wait for its final reply to leave the USB FIFO. */
    memset(user, 0, 24);
    assert(sys_usb_ctl(&p, GZ_USB_WRITE, 0, 24) == 24);
    assert(sys_usb_ctl(&p, GZ_USB_REBOOT_CE, 0, 0) == GZ_ETIMEDOUT && !rebooted);
    assert(usbf_host_in(&model, answer, sizeof(answer)) == 24);
    usb_poll();
    if (!setjmp(reboot_return)) { sys_usb_ctl(&p, GZ_USB_REBOOT_CE, 0, 0); assert(0); }
    assert(rebooted == 1);
    uint32_t old = generation; set_lines(0); assert(configured && generation != old);
    set_lines(3); assert(lines == 3);
    usbf_host_bus_reset(&model); usb_poll(); assert(!configured && !tx_busy && rh == rt);
    assert(net_stops && sys_usb_ctl(&p,GZ_USB_NET_OPEN,0,0)==GZ_EDEAD);
    assert(sys_usb_ctl(&p,GZ_USB_NET_CLOSE,0,0)==456); /* cleanup works after unplug */
    uint32_t cancels=input_cancels;
    gu_put32(user,1); assert(sys_usb_ctl(&p,GZ_USB_DEBUG_TRANSPORT,0,4)==0);
    clear_session(); assert(input_cancels==cancels); /* cable reset cannot cancel network input */
    gu_put32(user,0); assert(sys_usb_ctl(&p,GZ_USB_DEBUG_TRANSPORT,0,4)==0);
    clear_session(); assert(input_cancels==cancels+1);
    usb_stop(); assert(!active);
    puts("usb-driver: ALL PASS"); return 0;
}
