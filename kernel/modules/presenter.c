#include "../module.h"
int presenter_init(uint32_t, unsigned);
void presenter_update(void), presenter_stop(bool);
const struct presenter_driver glaze_driver = {
    PRESENTER_DRIVER_MAGIC, PRESENTER_DRIVER_ABI, sizeof(struct presenter_driver),
    presenter_init, presenter_update, presenter_stop
};
