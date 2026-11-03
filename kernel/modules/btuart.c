#include "../module.h"
int btuart_init(void), btuart_error(void);
void btuart_poll(void), btuart_stop(void), btuart_consume(uint32_t);
int btuart_read(uint8_t *, uint32_t), btuart_write(const uint8_t *, uint32_t);
const struct bt_driver glaze_driver = {
    BT_DRIVER_MAGIC, BT_DRIVER_ABI, sizeof(struct bt_driver),
    btuart_init, btuart_poll, btuart_stop, btuart_error, btuart_read, btuart_consume, btuart_write
};
