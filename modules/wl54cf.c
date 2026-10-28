#include "../kernel/drivers.h"
#include "../kernel/module.h"
static bool present(void) { return true; } /* init result is retained by dispatch */
const struct net_driver glaze_driver = {
    .magic = NET_DRIVER_MAGIC, .abi = NET_DRIVER_ABI,
    .size = sizeof(struct net_driver), .kind = GZ_NET_WL54CF,
    .init = wl54cf_init, .present = present, .irq = wl54cf_irq, .stop = wl54cf_removed,
    .send_frame = wl54cf_send, .control = wl54cf_control
};
