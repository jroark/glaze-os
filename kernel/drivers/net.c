/*
 * Network cards and the raw-frame interface shared by their drivers.
 *
 * One card owns the primary socket until it is pulled: found at boot by
 * net_init(), or inserted later (cf.c's pcmcia_poll(), which powers and
 * configures it, then calls net_insert()). A card is identified by its CIS
 * before any of its registers are touched, and only a network card (FUNCID
 * 6) gets a network driver: the XI-825 and WL54-CF by MANFID, anything else
 * the NE2000 driver, which checks the DP8390 and its station address before
 * claiming it (NE2000 clones carry many MANFIDs).
 *
 * The NE2000 and WL54-CF hand received frames to the queue here; the XI-825
 * keeps its own, with its Wi-Fi controls (xi825.c).
 */
#include <glaze/wifi.h>
#include "../module.h"
#include "../hwfile.h"
#ifndef NET_TEST
#include "../drivers.h"
#include "../hw.h"
#include "../kernel.h"
#include "../mips.h"
#endif

#define QUEUE_LEN 8
static unsigned kind;           /* GZ_NET_*: the card that owns the socket */
static bool claimed, online;
/* A driver or firmware file from an installed hardware package is still
 * coming from the file server (hwfile.c): the service worker asks again. Or
 * no one has the driver: look again when the installed packages change. */
static bool pending, missing;
static uint32_t missing_generation;
static unsigned card_manufacturer, card_product;
static int bound_port;
static struct gz_netif_info info;
static struct module image;
static const struct net_driver *driver;
static uint32_t generation;
struct frame { uint16_t length; uint8_t data[1536]; };
static struct frame *queue;
static uint8_t *tx_buf;
#define QUEUE_PAGES ALIGN_UP(QUEUE_LEN * sizeof(struct frame) + 1536u, PAGE_SIZE) / PAGE_SIZE
static uint32_t head, tail;

static int net_access(const struct proc *p)
{
    if (!(p->flags & PROC_NET)) return GZ_EPERM;
    return p->net_generation == generation ? 0 : GZ_EDEAD;
}

/* IDs survive service restarts; a stale socket can never alias a new one. */
static uint32_t next_handle = 1;
int net_handle(struct proc *p)
{
    int error = net_access(p);
    if (error) return error;
    if (next_handle >= 0x7fffffffu) return GZ_EFULL;
    return (int)next_handle++;
}

uint32_t net_generation(void) { return generation; }
uint32_t net_driver_kind(void) { return usbnet_up() ? GZ_NET_USB : driver ? kind : GZ_NET_NONE; }
uint32_t net_driver_pages(void) { return image.pages + (queue ? QUEUE_PAGES : 0) + usbnet_pages(); }

/* One IP interface owns routing. USB takes precedence while its host helper
 * is attached; changing transport invalidates old raw-frame handles and
 * restarts the network service through its normal generation policy. */
void net_usb_changed(void)
{
    generation++;
    head = tail = 0;
    if (bound_port > 0) ipc_notify(bound_port, GZ_NOTIFY_NET);
    bound_port = 0;
}

void net_set_mac(const uint8_t mac[6]) { memcpy(info.mac, mac, 6); }
void net_rx_drop(void) { info.rx_dropped++; }
void net_overrun(void) { info.overruns++; }
void net_receive(const uint8_t *frame, uint32_t len)
{
    if (usbnet_up()) return; /* Never inject card traffic into the USB subnet. */
    if (!queue || len < 14 || len > sizeof(queue[0].data) || head - tail == QUEUE_LEN) {
        net_rx_drop();
        return;
    }
    memcpy(queue[head % QUEUE_LEN].data, frame, len);
    queue[head % QUEUE_LEN].length = (uint16_t)len;
    head++;
    info.rx_frames++;
    if (bound_port > 0)
        ipc_notify(bound_port, GZ_NOTIFY_NET);
}

bool net_up(void)
{
    return usbnet_up() || (online && driver && driver->present());
}

static void free_queue(void)
{
    if (queue) {
        memset(queue, 0, QUEUE_PAGES * PAGE_SIZE);
        for (uint32_t i = 0; i < QUEUE_PAGES; i++)
            page_free((uint8_t *)queue + i * PAGE_SIZE);
    }
    queue = NULL;
    tx_buf = NULL;
}

/* Files a driver needs before init, which must never wait for them. */
static int prefetch(const char *path)
{
    struct hwfile f;
    int error = hwfile_open(path, &f);
    if (!error) hwfile_close(&f);
    return error;
}

/* Loads and starts the driver for the card select_card() claimed. */
static void attach(void)
{
    unsigned manufacturer = card_manufacturer, product = card_product;
    const char *path;
    bool was_pending = pending;
    pending = missing = false;
    if (manufacturer == 0xd601 && product == 5) {
        kind = GZ_NET_XI825; path = "drivers/xi825.ko";
    } else if (manufacturer == 0x02df && product == 0x8103) {
        kind = GZ_NET_WL54CF; path = "drivers/wl54cf.ko";
    } else if (manufacturer == 0x0101 && product == 0x0589) {
        kind = GZ_NET_3C589; path = "drivers/3c589.ko";     /* Linux 3c589_cs */
    } else if (manufacturer == 0x0156 && product == 0x0002) {
        kind = GZ_NET_HERMES; path = "drivers/hermes.ko";   /* Lucent/Agere Orinoco */
    } else if (manufacturer == 0x0101 || manufacturer == 0x0105 || manufacturer == 0x0128 ||
               manufacturer == 0x0156) {
        /* 3Com, Xircom, Megahertz and Lucent cards that aren't NE2000
         * clones: NE2000 register writes could wedge them. Keep the socket
         * reserved so ATA never touches them either. */
        kprintf("net: unsupported network card %x:%x; socket reserved\n", manufacturer, product);
        kind = GZ_NET_NONE;
        driver = NULL;
        online = false;
        return;
    } else {
        kind = GZ_NET_NE2000; path = "drivers/ne2000.ko";
    }
    int error = 0;
    if (kind == GZ_NET_WL54CF) {
        /* A missing pair is the driver's to report; a pending one is waited for. */
        int a = prefetch("firmware/cf8385_helper.bin"), b = prefetch("firmware/cf8385.bin");
        if (a == GZ_EINPROGRESS || b == GZ_EINPROGRESS) error = GZ_EINPROGRESS;
    }
    if (!error) error = module_load(path, "glaze_driver", &image);
    if (error == GZ_EINPROGRESS) {
        if (!was_pending) kprintf("net: %s waits for an installed package\n", path);
        pending = true;
        return;
    }
    if (error) goto failed;
    const struct net_driver *d = image.entry;
    if (((uintptr_t)d & 3u) || !module_contains(&image, d, sizeof(*d)) || d->magic != NET_DRIVER_MAGIC ||
        d->abi != NET_DRIVER_ABI || d->size != sizeof(*d) || d->kind != kind ||
        !module_executable(&image, (const void *)d->init) ||
        !module_executable(&image, (const void *)d->present) ||
        !module_executable(&image, (const void *)d->stop) ||
        !module_executable(&image, (const void *)d->irq)) {
        error = GZ_ECORRUPT; goto failed;
    }
    const void *callbacks[] = {
        d->init, d->present, d->stop, d->irq, d->send_frame, d->control,
        d->info, d->bind, d->send, d->recv, d->wifi
    };
    for (unsigned i = 0; i < ARRAY_SIZE(callbacks); i++)
        if (callbacks[i] && !module_executable(&image, callbacks[i])) {
            error = GZ_ECORRUPT; goto failed;
        }
    if (kind != GZ_NET_XI825 && kind != GZ_NET_HERMES) {
        queue = page_alloc_n(QUEUE_PAGES);
        if (!queue) { error = GZ_ENOMEM; goto failed; }
        tx_buf = (uint8_t *)queue + QUEUE_LEN * sizeof(*queue);
        if (!d->send_frame) { error = GZ_ECORRUPT; goto failed; }
    } else if (!d->info || !d->bind || !d->send || !d->recv || !d->wifi) {
        error = GZ_ECORRUPT; goto failed;
    }
    driver = d;
    if (!(online = driver->init())) {
        driver->stop();
        error = GZ_EIO; goto failed;
    }
    info.present = true;
    info.kind = (uint16_t)kind;
    hwfile_trim();
    return;
failed:
    driver = NULL;
    online = false;
    free_queue();
    module_unload(&image);
    hwfile_trim();
    if (error == GZ_ENOENT) {
        missing = true;
        missing_generation = hwfile_generation();
    }
    kprintf("net: %s unavailable (%d); socket reserved\n", path, error);
}

static void select_card(unsigned manufacturer, unsigned product)
{
    if (claimed) net_removed();
    claimed = true;
    generation++;
    head = tail = 0;
    memset(&info, 0, sizeof(info));
    card_manufacturer = manufacturer;
    card_product = product;
    attach();
}

/* The service worker's pass: a driver that was being fetched, or one that
 * a newly installed package may now supply. */
void net_retry(void)
{
    if (claimed && !driver && (pending || (missing && missing_generation != hwfile_generation())))
        attach();
}

void net_init(void)
{
    if (driver)
        return;  /* The cold socket initializer already configured this card. */
    /* Finds the socket holding a card and points the windows at it (cf.c). */
    bool present = PCMCIA_CARD_PRESENT();
    /* The C000/D000 card windows are the CF socket's; the jacket's
     * (socket 1) are elsewhere and CE leaves this bit clear for it. */
    if (pcmcia_io_base == PCMCIA0_IO)
        PCMCIA_BRIDGE |= PCMCIA_BRIDGE_EN;
    if (!present)
        return;
    unsigned function = 0xff, manufacturer = 0, product = 0;
    for (unsigned i = 0, n = 0; n < 64 && i + 2 < 0x100; n++) {
        unsigned code = CF_CIS(i);
        if (code == 0xff) break;
        if (!code) { i++; continue; }
        unsigned len = CF_CIS(i + 1);
        if (len == 0xff || i + 2 + len > 0x100) break;
        if (code == 0x21 && len >= 2) function = CF_CIS(i + 2);
        if (code == 0x20 && len >= 4) {
            manufacturer = CF_CIS(i + 2) | CF_CIS(i + 3) << 8;
            product = CF_CIS(i + 4) | CF_CIS(i + 5) << 8;
        }
        i += 2 + len;
    }
    /* Never probe a storage card's registers with NIC writes. */
    if (function == 6)
        select_card(manufacturer, product);
}

bool net_insert(unsigned manufacturer, unsigned product)
{
    select_card(manufacturer, product);
    /* The socket power-up caller needs the card's result, even if USB
     * independently supplies a working network connection. A driver still
     * on its way, or not installed, won't come from powering up again. */
    return (online && driver && driver->present()) || pending || missing;
}

/* Time at each GZ_CARD_POWER_* level, for Settings → Battery. */
static uint8_t power_noted, power_now;
static uint64_t power_at, power_us[GZ_CARD_POWER_OFF + 1];
static uint32_t irqs;
static void power_account(void);
void net_power_note(unsigned state)
{
    if (state > GZ_CARD_POWER_OFF || state == power_noted) return;
    power_account();            /* the time so far was in the old state */
    power_noted = (uint8_t)state;
    power_account();
}
static void power_account(void)
{
    uint64_t now = time_us();
    if (power_at) power_us[power_now] += now - power_at;
    power_at = now;
    power_now = !driver ? GZ_CARD_POWER_NONE : !online ? GZ_CARD_POWER_OFF :
                power_noted ? power_noted : GZ_CARD_POWER_FULL;
}
void net_power_status(struct gz_power_status *st)
{
    power_account();
    st->card_power = power_now;
    st->card_full_ms = (uint32_t)(power_us[GZ_CARD_POWER_FULL] / 1000u);
    st->card_save_ms = (uint32_t)(power_us[GZ_CARD_POWER_SAVE] / 1000u);
    st->card_doze_ms = (uint32_t)(power_us[GZ_CARD_POWER_DOZE] / 1000u);
    st->card_off_ms = (uint32_t)((power_us[GZ_CARD_POWER_RADIO_OFF] + power_us[GZ_CARD_POWER_OFF]) / 1000u);
    st->net_irqs = irqs;
}

/* The card was pulled: its driver forgets it, and its frames go. */
void net_removed(void)
{
    if (driver) driver->stop();
    power_account();
    driver = NULL;
    online = claimed = pending = missing = false;
    power_noted = 0;
    power_account();            /* from now on, no card */
    generation++;
    kind = GZ_NET_NONE;
    head = tail = 0;
    memset(&info, 0, sizeof(info));
    if (bound_port > 0) ipc_notify(bound_port, GZ_NOTIFY_NET);
    bound_port = 0;
    free_queue();
    module_unload(&image);
    hwfile_trim();          /* anything fetched for it */
}

/* Claimed stays true if a recognized card failed initialization: the ATA
 * driver must never reset or identify it as a storage device. */
bool net_present(void) { return claimed; }

void net_irq(void)
{
    /* From the tick and GIRQ0 alike: a pulled card reads 0xFF everywhere,
     * which a driver would take for every event at once. pcmcia_poll()
     * notices the removal itself. */
    if (!PCMCIA_CARD_PRESENT())
        return;
    if (driver && online) {
        irqs++;
        driver->irq();
    }
    power_account();
}

static int check(struct proc *p)
{
    int error = net_access(p);
    if (error) return error;
    return info.present ? 0 : GZ_ENOENT;
}

int sys_net_info(struct proc *p, uint32_t uout)
{
    if (!(p->flags & PROC_NET))
        return GZ_EPERM;
    pcmcia_poll();          /* the network server's look at the socket */
    int error = net_access(p);
    if (error) return error;
    if (usbnet_up()) return usbnet_info(p, uout);
    if (driver && driver->info)
        return driver->info(p, uout);
    return copy_to_as(&p->as, uout, &info, sizeof(info));
}

int sys_net_bind(struct proc *p, int port)
{
    int error = net_access(p);
    if (error) return error;
    if (usbnet_up()) return usbnet_bind(port);
    if (driver && driver->bind)
        return driver->bind(p, port);
    int rv = check(p);
    if (rv)
        return rv;
    bound_port = port;
    if (head != tail)
        ipc_notify(port, GZ_NOTIFY_NET);
    return 0;
}

int sys_net_send(struct proc *p, uint32_t ubuf, uint32_t len)
{
    int error = net_access(p);
    if (error) return error;
    if (usbnet_up()) return usbnet_send(p, ubuf, len);
    if (driver && driver->send)
        return driver->send(p, ubuf, len);
    int rv = check(p);
    if (rv)
        return rv;
    if (len < 14 || len > GZ_ETH_FRAME_MAX)
        return GZ_EINVAL;
    if (copy_from_as(&p->as, tx_buf, ubuf, len))
        return GZ_EINVAL;
    if (len < 60) {
        memset(tx_buf + len, 0, 60 - len);
        len = 60;
    }
    rv = driver->send_frame(tx_buf, len);
    if (rv >= 0)
        info.tx_frames++;
    return rv;
}

int sys_net_recv(struct proc *p, uint32_t ubuf, uint32_t max)
{
    int error = net_access(p);
    if (error) return error;
    if (usbnet_up()) return usbnet_recv(p, ubuf, max);
    if (driver && driver->recv)
        return driver->recv(p, ubuf, max);
    int rv = check(p);
    if (rv)
        return rv;
    if (head == tail)
        return 0;
    uint32_t n = MIN(queue[tail % QUEUE_LEN].length, max);
    if (copy_to_as(&p->as, ubuf, queue[tail % QUEUE_LEN].data, n))
        return GZ_EINVAL;
    tail++;
    return (int)n;
}

int sys_wifi_ctl(struct proc *p, uint32_t op, uint32_t address)
{
    int error = net_access(p);
    if (error) return error;
    if (usbnet_up()) return GZ_ENOENT;
    if (driver && driver->wifi)
        return driver->wifi(p, op, address);
    if (!driver || !driver->control || !info.present)
        return GZ_ENOENT;
    union { struct wifi_status status; struct wifi_config config; struct wifi_request request;
            struct wifi_key key; struct wifi_power power; } data;
    unsigned n = op == WIFI_CONFIGURE ? sizeof(data.config) :
                 op == WIFI_POWER ? sizeof(data.power) :
                 op == WIFI_SET_KEY ? sizeof(data.key) :
                 op == WIFI_CONNECT ? sizeof(data.request) : 0;
    memset(&data, 0, sizeof(data));
    if (n && copy_from_as(&p->as, &data, address, n))
        return GZ_EINVAL;
    /* Commands and the runtime IRQ share the command/response FIFO. */
#ifndef NET_TEST
    uint32_t flags = irq_save();
#endif
    int rv = driver->control(op, &data);
    if (op == WIFI_CONNECT || op == WIFI_CONFIGURE || op == WIFI_DISCONNECT)
        head = tail = 0;
#ifndef NET_TEST
    irq_restore(flags);
#endif
    if (!rv && op == WIFI_STATUS)
        rv = copy_to_as(&p->as, address, &data.status, sizeof(data.status));
    memset(&data, 0, sizeof(data));
    return rv;
}
