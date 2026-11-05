#include "../drivers.h"
#include "../module.h"
const struct net_driver glaze_driver = {
    .magic = NET_DRIVER_MAGIC, .abi = NET_DRIVER_ABI,
    .size = sizeof(struct net_driver), .kind = GZ_NET_3C589,
    .init = el3_init, .present = el3_present, .irq = el3_irq, .stop = el3_removed,
    .send_frame = el3_send
};
