#include "../drivers.h"
#include "../module.h"
const struct net_driver glaze_driver = {
    .magic = NET_DRIVER_MAGIC, .abi = NET_DRIVER_ABI,
    .size = sizeof(struct net_driver), .kind = GZ_NET_MODEM,
    .init = modem_init, .present = modem_present, .irq = modem_irq, .stop = modem_removed,
    .info = modem_info, .bind = modem_bind, .send = modem_send,
    .recv = modem_recv, .wifi = modem_control
};
