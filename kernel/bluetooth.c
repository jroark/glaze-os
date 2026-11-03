/* Trusted UART module lifecycle; the socket dispatcher verifies CIS first. */
#include "kernel.h"
#include "drivers.h"
#include "module.h"
#include "hw.h"
#include <glaze/bluetooth.h>
static struct module image;
static const struct bt_driver *driver;
static bool claimed;
static uint32_t generation;
uint32_t bt_generation(void) { return generation; }
bool bt_present(void) { return driver && PCMCIA_CARD_PRESENT(); }
bool bt_ready(void) { return driver && PCMCIA_CARD_PRESENT() && !driver->error(); }
void bt_poll(void) { if (driver && PCMCIA_CARD_PRESENT()) driver->poll(); }
void bt_removed(void)
{
    if (!claimed) return;
    if (driver) driver->stop();
    driver = NULL; claimed = false; generation++;
    module_unload(&image);
}
void bt_insert(void)
{
    if (claimed) return;
    claimed = true; generation++;
    int rv = module_load("drivers/btuart.ko", "glaze_driver", &image);
    if (rv) goto failed;
    const struct bt_driver *d = image.entry;
    if (((uintptr_t)d & 3u) || !module_contains(&image, d, sizeof(*d)) ||
        d->magic != BT_DRIVER_MAGIC || d->abi != BT_DRIVER_ABI || d->size != sizeof(*d)) goto failed;
    const void *callbacks[] = {d->init,d->poll,d->stop,d->error,d->read,d->consume,d->write};
    for (unsigned i = 0; i < sizeof(callbacks)/sizeof(callbacks[0]); i++)
        if (!module_executable(&image, callbacks[i])) goto failed;
    if (d->init()) { d->stop(); goto failed; }
    driver = d; kputs("bluetooth: BCSP UART ready\n"); return;
failed:
    module_unload(&image); kputs("bluetooth: UART module unavailable; socket reserved\n");
}
int sys_bt_io(struct proc *p, uint32_t op, uint32_t buffer, uint32_t size)
{
    if (!(p->flags & PROC_BLUETOOTH)) return GZ_EPERM;
    if (p->bt_generation != generation) return GZ_EDEAD;
    if (!driver || !PCMCIA_CARD_PRESENT()) return GZ_ENOENT;
    if (op == BT_IO_RESET) {
        if (size) return GZ_EINVAL;
        driver->stop(); return driver->init();
    }
    if (!size || size > 256) return GZ_EINVAL;
    uint8_t bytes[256];
    bt_poll();
    if (op == BT_IO_WRITE) {
        if (copy_from_as(&p->as, bytes, buffer, size)) return GZ_EINVAL;
        return driver->write(bytes, size);
    }
    if (op == BT_IO_READ) {
        int n = driver->read(bytes, size);
        if (n > 0) {
            if (copy_to_as(&p->as, buffer, bytes, (uint32_t)n)) return GZ_EINVAL;
            driver->consume((uint32_t)n);
        }
        return n;
    }
    return GZ_EINVAL;
}
