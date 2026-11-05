/* Loads and validates drivers/cfuart.ko for a CF serial card (bluetooth.c, modem.c). */
#ifndef GLAZE_UART_CARD_H
#define GLAZE_UART_CARD_H
#include "module.h"
static inline const struct uart_driver *uart_card_load(struct module *image)
{
    if (module_load("drivers/cfuart.ko", "glaze_driver", image)) return NULL;
    const struct uart_driver *d = image->entry;
    if (((uintptr_t)d & 3u) || !module_contains(image, d, sizeof(*d)) ||
        d->magic != UART_DRIVER_MAGIC || d->abi != UART_DRIVER_ABI || d->size != sizeof(*d))
        goto bad;
    const void *callbacks[] = {d->init,d->poll,d->stop,d->error,d->read,d->consume,d->write,
                               d->set_lines,d->status};
    for (unsigned i = 0; i < sizeof(callbacks)/sizeof(callbacks[0]); i++)
        if (!module_executable(image, callbacks[i])) goto bad;
    return d;
bad:
    module_unload(image);
    return NULL;
}
#endif
