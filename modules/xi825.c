#include "../kernel/drivers.h"
#include "../kernel/module.h"
static bool start(void) { xi825_init(); return xi825_present(); }
const struct net_driver glaze_driver = {
    .magic = NET_DRIVER_MAGIC, .abi = NET_DRIVER_ABI,
    .size = sizeof(struct net_driver), .kind = GZ_NET_XI825,
    .init = start, .present = xi825_present, .irq = xi825_irq, .stop = xi825_removed,
    .info = xi825_info, .bind = xi825_bind, .send = xi825_send,
    .recv = xi825_recv, .wifi = xi825_control
};
