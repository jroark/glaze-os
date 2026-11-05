/* Trusted UART module lifecycle; the socket dispatcher verifies CIS first. */
#include "kernel.h"
#include "drivers.h"
#include "module.h"
#include "uart_card.h"
#include "hw.h"
#include <glaze/bluetooth.h>
static struct module image;
static const struct uart_driver *driver;
/* Verified CIS entry 3: I/O 3f8h, divisor 1, 8E1, FIFO trigger one (BLUETOOTH.md). */
static const struct uart_config config = {
    .cor_address = 0xe0, .io_base = 0x3f8, .divisor = 1,
    .cor = 0x43, .lcr = 0x1b, .mcr = 0x0b, .fcr = 7,
};
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
    const struct uart_driver *d = uart_card_load(&image);
    if (!d) goto failed;
    if (d->init(&config)) { d->stop(); goto failed; }
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
        driver->stop(); return driver->init(&config);
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
