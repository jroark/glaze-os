#include "../kernel/drivers.h"
#include "../kernel/kernel.h"
#include <assert.h>
#include <glaze/usb.h>
#include <stdio.h>
#include <string.h>
static uint8_t user[4096];
static uint64_t clock_ms;
static struct gz_input_event output[128];
static unsigned emitted;
uint64_t time_us(void)
{
    return clock_ms * 1000;
}
void input_push(uint16_t t, uint16_t c, int x, int y)
{
    assert(emitted < 128);
    output[emitted++] = (struct gz_input_event){t, c, x, y, (uint32_t)clock_ms};
}
int copy_from_as(struct addrspace *as, void *p, uint32_t va, uint32_t n)
{
    (void)as;
    if (va > sizeof(user) || n > sizeof(user) - va)
        return -1;
    memcpy(p, user + va, n);
    return 0;
}
int copy_to_as(struct addrspace *as, uint32_t va, const void *p, uint32_t n)
{
    (void)as;
    if (va > sizeof(user) || n > sizeof(user) - va)
        return -1;
    memcpy(user + va, p, n);
    return 0;
}
#include "../kernel/usb_input.c"
static struct proc usb = {.flags = PROC_USB}, denied;
static int call(unsigned op, unsigned n)
{
    return usb_input_ctl(&usb, op, 0, n);
}
static void event(unsigned off, unsigned ms, unsigned type, unsigned code, int x, int y)
{
    uint8_t *p = user + 4 + off * 12;
    gu_put32(p, ms);
    p[4] = type;
    p[5] = 0;
    p[6] = code;
    p[7] = 0;
    p[8] = x;
    p[9] = (uint16_t)x >> 8;
    p[10] = y;
    p[11] = (uint16_t)y >> 8;
}
int main(void)
{
    assert(usb_input_ctl(&denied, GZ_USB_SCRIPT_CANCEL, 0, 0) == GZ_EPERM);
    gu_put32(user, GU_SCRIPT_MAX + 1);
    assert(call(GZ_USB_SCRIPT_BEGIN, 4) == GZ_EINVAL);
    gu_put32(user, 3);
    assert(!call(GZ_USB_SCRIPT_BEGIN, 4));
    assert(call(GZ_USB_SCRIPT_START, 0) == GZ_EINVAL);
    gu_put32(user, 0);
    event(0, 0, GZ_EV_TOUCH_DOWN, 0, 251, 331);
    event(1, 40, GZ_EV_TEXT, 'a', 0, 0);
    event(2, 80, GZ_EV_TOUCH_UP, 0, 251, 331);
    assert(!call(GZ_USB_SCRIPT_DATA, 40));
    assert(!call(GZ_USB_SCRIPT_START, 0));
    clock_ms = 99;
    usb_input_poll();
    assert(!emitted);
    clock_ms = 100;
    usb_input_poll();
    assert(emitted == 1 && output[0].x == 251 && output[0].y == 331);
    clock_ms = 139;
    usb_input_poll();
    assert(emitted == 1);
    clock_ms = 180;
    usb_input_poll();
    assert(emitted == 3);
    assert(call(GZ_USB_SCRIPT_STATUS, 16) == 16 && gu_get32(user) == GU_SCRIPT_DONE &&
           gu_get32(user + 8) == 3);
    gu_put32(user, 2);
    assert(!call(GZ_USB_SCRIPT_BEGIN, 4));
    gu_put32(user, 0);
    event(0, 20, GZ_EV_KEY_DOWN, GZ_KEY_HOME, 0, 0);
    event(1, 10, GZ_EV_KEY_UP, GZ_KEY_HOME, 0, 0);
    assert(call(GZ_USB_SCRIPT_DATA, 28) == GZ_EINVAL && loaded == 0);
    event(0, 0, GZ_EV_TOUCH_DOWN, 0, 1, 2);
    event(1, 1000, GZ_EV_TOUCH_UP, 0, 1, 2);
    assert(!call(GZ_USB_SCRIPT_DATA, 28));
    assert(!call(GZ_USB_SCRIPT_START, 0));
    clock_ms += 100;
    usb_input_poll();
    assert(touch);
    usb_input_cancel();
    assert(!touch && output[emitted - 1].type == GZ_EV_TOUCH_CANCEL);
    gu_put32(user, 2);
    assert(!call(GZ_USB_SCRIPT_BEGIN, 4));
    gu_put32(user, 0);
    event(0, 0, GZ_EV_KEY_DOWN, GZ_KEY_ENTER, 0, 0);
    event(1, 1000, GZ_EV_KEY_UP, GZ_KEY_ENTER, 0, 0);
    assert(!call(GZ_USB_SCRIPT_DATA, 28));
    assert(!call(GZ_USB_SCRIPT_START, 0));
    clock_ms += 100;
    usb_input_poll();
    usb_input_cancel();
    assert(output[emitted - 1].type == GZ_EV_KEY_UP && output[emitted - 1].code == GZ_KEY_ENTER);
    gu_put32(user, 1);
    assert(!call(GZ_USB_SCRIPT_BEGIN, 4));
    gu_put32(user, 0);
    event(0, 0, GZ_EV_TOUCH_DOWN, 0, 142, 280);
    assert(!call(GZ_USB_SCRIPT_DATA, 16));
    assert(!call(GZ_USB_SCRIPT_START, 0));
    clock_ms += 100;
    usb_input_poll();
    assert(state == GU_SCRIPT_DONE && touch);
    usb_input_cancel();
    assert(!touch && output[emitted - 1].type == GZ_EV_TOUCH_CANCEL);
    clock_ms = 0xfffffff0u;
    gu_put32(user, 1);
    assert(!call(GZ_USB_SCRIPT_BEGIN, 4));
    gu_put32(user, 0);
    event(0, 0, GZ_EV_TEXT, 'z', 0, 0);
    assert(!call(GZ_USB_SCRIPT_DATA, 16));
    assert(!call(GZ_USB_SCRIPT_START, 0));
    unsigned before = emitted;
    clock_ms += 99;
    usb_input_poll();
    assert(emitted == before);
    clock_ms++;
    usb_input_poll();
    assert(emitted == before + 1 && output[before].code == 'z');
    gu_put32(user, 0);
    assert(!call(GZ_USB_SCRIPT_BEGIN, 4) && state == GU_SCRIPT_READY);
    assert(!call(GZ_USB_SCRIPT_START, 0));
    clock_ms += 100;
    usb_input_poll();
    assert(state == GU_SCRIPT_DONE && count == 0);
    puts("usb-input: ALL PASS");
    return 0;
}
