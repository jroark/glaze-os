/* Bounded Ethernet queues, shared by PROC_USB and the selected PROC_NET.
 * USB framing lives in userspace; no allocations occur on the USB IRQ path.
 * The proven endpoints/register sequence remain in usb.c (BE300_USB_FUNCTION.md).
 */
#include "../kernel.h"
#include "../drivers.h"
#include <glaze/usb.h>
#include <glaze/usb_net.h>

struct usbnet_frame { uint32_t size; uint8_t data[GUN_FRAME_MAX]; };
struct usbnet_queue { uint32_t head, tail; struct usbnet_frame frames[GUN_QUEUE]; };
struct usbnet_storage { struct usbnet_queue rx, tx; };
#define USBNET_PAGES (ALIGN_UP(sizeof(struct usbnet_storage), PAGE_SIZE) / PAGE_SIZE)
static struct usbnet_storage *storage;
static bool enabled;
static int notify_port;
static struct gz_netif_info usb_info;

bool usbnet_up(void) { return enabled; }
uint32_t usbnet_pages(void) { return storage ? USBNET_PAGES : 0; }

void usbnet_stop(void)
{
    if (!enabled) return;
    enabled = false;
    if (notify_port > 0) ipc_notify(notify_port, GZ_NOTIFY_NET);
    notify_port = 0;
    /* Keep the allocation until a userspace control call: a bus reset may
     * arrive in a timer IRQ. Stop makes its contents immediately unreachable. */
    net_usb_changed();
}
static void release(void)
{
    if (!storage) return;
    memset(storage, 0, USBNET_PAGES * PAGE_SIZE);
    for (unsigned i = 0; i < USBNET_PAGES; i++)
        page_free((uint8_t *)storage + i * PAGE_SIZE);
    storage = NULL;
}
void usbnet_collect(void) { if (!enabled) release(); }
int usbnet_info(struct proc *p, uint32_t out)
{ return copy_to_as(&p->as, out, &usb_info, sizeof(usb_info)); }
int usbnet_bind(int port)
{
    notify_port = port;
    if (storage->rx.head != storage->rx.tail) ipc_notify(port, GZ_NOTIFY_NET);
    return 0;
}
static int dequeue(struct proc *p, struct usbnet_queue *q, uint32_t out, uint32_t max)
{
    if (q->head == q->tail) return 0;
    struct usbnet_frame *f = &q->frames[q->tail % GUN_QUEUE];
    if (max < f->size) return GZ_EINVAL; /* Never truncate an Ethernet packet. */
    if (copy_to_as(&p->as, out, f->data, f->size)) return GZ_EINVAL;
    q->tail++;
    return (int)f->size;
}
int usbnet_recv(struct proc *p, uint32_t out, uint32_t max)
{ return dequeue(p, &storage->rx, out, max); }
int usbnet_send(struct proc *p, uint32_t in, uint32_t len)
{
    if (len < 14 || len > GUN_FRAME_MAX) return GZ_EINVAL;
    struct usbnet_queue *q = &storage->tx;
    if (q->head - q->tail == GUN_QUEUE) return GZ_EFULL;
    struct usbnet_frame *f = &q->frames[q->head % GUN_QUEUE];
    if (copy_from_as(&p->as, f->data, in, len)) return GZ_EINVAL;
    if (len < 60) memset(f->data + len, 0, 60 - len);
    f->size = MAX(len, 60u); q->head++; usb_info.tx_frames++;
    return (int)len;
}
int usbnet_ctl(struct proc *p, uint32_t op, uint32_t address, uint32_t len)
{
    if (op == GZ_USB_NET_CLOSE) {
        if (len) return GZ_EINVAL;
        usbnet_stop(); release(); return 0;
    }
    if (op == GZ_USB_NET_OPEN) {
        if (len) return GZ_EINVAL;
        if (enabled) return GZ_EBUSY;
        release();
        storage = page_alloc_n(USBNET_PAGES);
        if (!storage) return GZ_ENOMEM;
        memset(storage, 0, USBNET_PAGES * PAGE_SIZE);
        memset(&usb_info, 0, sizeof(usb_info));
        static const uint8_t mac[6] = GUN_DEVICE_MAC;
        memcpy(usb_info.mac, mac, sizeof(mac));
        usb_info.present = 1; usb_info.kind = GZ_NET_USB;
        enabled = true; net_usb_changed(); return 0;
    }
    /* Input: zero bytes to poll, or one complete Ethernet frame.
     * Output: LE32 accepted (0 backpressure / 1 queued), then at most one
     * complete device-to-host frame. Replay is handled by the USB service.
     * Caller provides GUN_FRAME_MAX+4 bytes even for an empty input. */
    if (op != GZ_USB_NET_EXCHANGE || !enabled) return GZ_EDEAD;
    if (len && (len < 14 || len > GUN_FRAME_MAX)) return GZ_EINVAL;
    uint8_t result[4 + GUN_FRAME_MAX];
    struct usbnet_queue *rx = &storage->rx, *tx = &storage->tx;
    bool accept = len && rx->head - rx->tail < GUN_QUEUE;
    struct usbnet_frame *incoming = &rx->frames[rx->head % GUN_QUEUE];
    if (accept && copy_from_as(&p->as, incoming->data, address, len)) return GZ_EINVAL;
    gu_put32(result, accept ? 1 : 0);
    unsigned count = 4;
    if (tx->head != tx->tail) {
        struct usbnet_frame *f = &tx->frames[tx->tail % GUN_QUEUE];
        memcpy(result + 4, f->data, f->size); count += f->size;
    }
    /* Commit both directions only after a successful copy: a bad user
     * pointer must not consume a packet or inject the same packet twice. */
    if (copy_to_as(&p->as, address, result, count)) return GZ_EINVAL;
    if (count > 4) tx->tail++;
    if (accept) {
        incoming->size = len; rx->head++; usb_info.rx_frames++;
        if (notify_port > 0) ipc_notify(notify_port, GZ_NOTIFY_NET);
    }
    return (int)count;
}
