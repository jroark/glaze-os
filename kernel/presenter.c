/* Presenter-to-Go module lifecycle. The service worker owns module loading;
 * removal never touches a departed card's registers. See PRESENTER.md. */
#include "kernel.h"
#include "drivers.h"
#include "module.h"
static struct module image;
static const struct presenter_driver *driver;
static uint32_t cor;
static int config_index;
static bool pending;
void presenter_insert(uint32_t address, int entry)
{
    if (driver || pending) return;
    cor = address; config_index = entry; pending = true;
}
void presenter_removed(void)
{
    pending = false;
    if (driver) driver->stop(false);
    driver = NULL;
    module_unload(&image);
}
bool presenter_active(void) { return driver != NULL || pending; }
void presenter_poll(void)
{
    if (pending) {
        if (!cor || (cor & 1) || cor >= 0x1000 || config_index <= 0 || config_index > 63) {
            kputs("presenter: no supported 2 KiB I/O configuration\n");
            pending = false;
            return;
        }
        int rv = module_load("drivers/presenter.ko", "glaze_driver", &image);
        if (rv == GZ_EINPROGRESS) return;
        pending = false;
        if (rv) { kprintf("presenter: module load failed %d\n", rv); return; }
        const struct presenter_driver *d = image.entry;
        if (((uintptr_t)d & 3u) || !module_contains(&image, d, sizeof(*d)) ||
            d->magic != PRESENTER_DRIVER_MAGIC || d->abi != PRESENTER_DRIVER_ABI ||
            d->size != sizeof(*d) || !module_executable(&image, d->init) ||
            !module_executable(&image, d->poll) || !module_executable(&image, d->stop)) {
            module_unload(&image);
            kputs("presenter: invalid module\n");
            return;
        }
        rv = d->init(cor, (unsigned)config_index);
        if (rv) { d->stop(true); module_unload(&image); kprintf("presenter: init failed %d\n", rv); return; }
        driver = d;
        kputs("presenter: initializing VGA 640x480 LCD mirror\n");
    }
    if (driver) driver->poll();
}
