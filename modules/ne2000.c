#include "../kernel/drivers.h"
#include "../kernel/module.h"
static bool start(void) { ne2000_init(); return ne2000_present(); }
const struct net_driver glaze_driver = {
    .magic = NET_DRIVER_MAGIC, .abi = NET_DRIVER_ABI,
    .size = sizeof(struct net_driver), .kind = GZ_NET_NE2000,
    .init = start, .present = ne2000_present, .irq = ne2000_irq, .stop = ne2000_removed,
    .send_frame = ne2000_send
};
