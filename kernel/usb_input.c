/* Timed debugger input uses the same event ring as emulator test builds.
 * Only the kernel-granted USB capability can configure this bounded queue. */
#include "drivers.h"
#include "kernel.h"
#include <glaze/sb.h>
#include <glaze/usb.h>
#include <glaze/mirror.h>

struct event {
    uint32_t ms;
    uint16_t type, code;
    int16_t x, y;
};
static struct event events[GU_SCRIPT_MAX];
static uint32_t count, loaded, pos, state, start, keys;
static bool touch;

void usb_input_cancel(void)
{
    if (touch)
        input_push(GZ_EV_TOUCH_CANCEL, 0, 0, 0);
    for (unsigned k = GZ_KEY_OK; k <= GZ_KEY_PAGEDOWN; k++)
        if (keys & (1u << k))
            input_push(GZ_EV_KEY_UP, k, 0, 0);
    touch = false;
    keys = 0;
    count = loaded = pos = 0;
    state = GU_SCRIPT_EMPTY;
}
void usb_input_poll(void)
{
    if (state != GU_SCRIPT_RUNNING)
        return;
    uint32_t now = (uint32_t)(time_us() / 1000u);
    if ((int32_t)(now - start) < 0)
        return;
    unsigned budget = 16;
    while (pos < count && events[pos].ms <= now - start && budget--) {
        struct event *e = &events[pos++];
        input_push(e->type, e->code, e->x, e->y);
        if (e->type == GZ_EV_TOUCH_DOWN)
            touch = true;
        if (e->type == GZ_EV_TOUCH_UP)
            touch = false;
        if (e->type == GZ_EV_KEY_DOWN)
            keys |= 1u << e->code;
        if (e->type == GZ_EV_KEY_UP)
            keys &= ~(1u << e->code);
    }
    if (pos == count) {
        /* Keep an explicit final down/move held for mid-gesture screenshots,
         * as emulator scripts do. CANCEL/session close releases it. */
        state = GU_SCRIPT_DONE;
    }
}
int usb_input_ctl(struct proc *p, uint32_t op, uint32_t buf, uint32_t len)
{
    if (!(p->flags & PROC_USB))
        return GZ_EPERM;
    uint8_t b[GU_PAYLOAD];
    if (op == GZ_USB_LIVE_INPUT) {
        if (state != GU_SCRIPT_EMPTY && state != GU_SCRIPT_DONE) return GZ_EBUSY;
        if (!len || len % 8 || len > GM_INPUT_BATCH * 8 || copy_from_as(&p->as,b,buf,len)) return GZ_EINVAL;
        /* Validate the entire batch before emitting any events. Reuse held
         * state so the existing disconnect/reset watchdog releases it. */
        for (unsigned i=0;i<len;i+=8) {
            unsigned type=b[i]|(unsigned)b[i+1]<<8, code=b[i+2]|(unsigned)b[i+3]<<8;
            int x=(int16_t)(b[i+4]|(unsigned)b[i+5]<<8),y=(int16_t)(b[i+6]|(unsigned)b[i+7]<<8);
            if (type < GZ_EV_TOUCH_DOWN || type > GZ_EV_TEXT ||
                (type <= GZ_EV_TOUCH_UP && (code || x<0 || x>=(int)GM_WIDTH || y<0 || y>=(int)GM_HEIGHT)) ||
                ((type==GZ_EV_KEY_DOWN || type==GZ_EV_KEY_UP) && (!code || code>GZ_KEY_PAGEDOWN)) ||
                (type==GZ_EV_TEXT && (!code || code>127))) return GZ_EINVAL;
        }
        for (unsigned i=0;i<len;i+=8) {
            unsigned type=b[i]|(unsigned)b[i+1]<<8,code=b[i+2]|(unsigned)b[i+3]<<8;
            int x=(int16_t)(b[i+4]|(unsigned)b[i+5]<<8),y=(int16_t)(b[i+6]|(unsigned)b[i+7]<<8);
            input_push(type,code,x,y);
            if(type==GZ_EV_TOUCH_DOWN)touch=true;
            if(type==GZ_EV_TOUCH_UP)touch=false;
            if(type==GZ_EV_KEY_DOWN)keys|=1u<<code;
            if(type==GZ_EV_KEY_UP)keys&=~(1u<<code);
        }
        return 0;
    }
    if (op == GZ_USB_SCRIPT_CANCEL) {
        if (len)
            return GZ_EINVAL;
        usb_input_cancel();
        return 0;
    }
    if (op == GZ_USB_SCRIPT_STATUS) {
        if (len != 16)
            return GZ_EINVAL;
        uint32_t now = (uint32_t)(time_us() / 1000u);
        uint32_t elapsed = state >= GU_SCRIPT_RUNNING && (int32_t)(now - start) >= 0 ? now - start : 0;
        gu_put32(b, state);
        gu_put32(b + 4, elapsed);
        gu_put32(b + 8, pos);
        gu_put32(b + 12, count);
        return copy_to_as(&p->as, buf, b, 16) ? GZ_EINVAL : 16;
    }
    if (op == GZ_USB_SCRIPT_START) {
        if (len || state != GU_SCRIPT_READY)
            return GZ_EINVAL;
        start = (uint32_t)(time_us() / 1000u) + 100;
        pos = 0;
        state = GU_SCRIPT_RUNNING;
        return 0;
    }
    if (len > sizeof(b) || copy_from_as(&p->as, b, buf, len))
        return GZ_EINVAL;
    if (op == GZ_USB_SCRIPT_BEGIN) {
        if (len != 4 || gu_get32(b) > GU_SCRIPT_MAX || state == GU_SCRIPT_RUNNING)
            return GZ_EINVAL;
        usb_input_cancel();
        count = gu_get32(b);
        state = count ? GU_SCRIPT_LOADING : GU_SCRIPT_READY;
        return 0;
    }
    if (op != GZ_USB_SCRIPT_DATA || state != GU_SCRIPT_LOADING || len < 16 || (len - 4) % 12 ||
        gu_get32(b) != loaded || (len - 4) / 12 > count - loaded)
        return GZ_EINVAL;
    uint32_t previous = loaded ? events[loaded - 1].ms : 0, added = (len - 4) / 12;
    for (uint32_t i = 0; i < added; i++) {
        const uint8_t *q = b + 4 + i * 12;
        struct event e = {gu_get32(q), q[4] | (uint16_t)q[5] << 8, q[6] | (uint16_t)q[7] << 8,
                          (int16_t)(q[8] | (uint16_t)q[9] << 8), (int16_t)(q[10] | (uint16_t)q[11] << 8)};
        if (e.ms < previous || e.ms > GU_SCRIPT_MS || e.type < GZ_EV_TOUCH_DOWN || e.type > GZ_EV_TEXT ||
            ((e.type == GZ_EV_KEY_DOWN || e.type == GZ_EV_KEY_UP) && (!e.code || e.code > GZ_KEY_PAGEDOWN)) ||
            (e.type == GZ_EV_TEXT && (!e.code || e.code > 127)))
            return GZ_EINVAL;
        events[loaded + i] = e;
        previous = e.ms;
    }
    loaded += added;
    if (loaded == count)
        state = GU_SCRIPT_READY;
    return 0;
}
