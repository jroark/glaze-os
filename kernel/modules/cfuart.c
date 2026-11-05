#include "../module.h"
int cfuart_init(const struct uart_config *), cfuart_error(void);
void cfuart_poll(void), cfuart_stop(void), cfuart_consume(uint32_t), cfuart_set_lines(uint8_t);
int cfuart_read(uint8_t *, uint32_t), cfuart_write(const uint8_t *, uint32_t);
uint8_t cfuart_status(void);
const struct uart_driver glaze_driver = {
    UART_DRIVER_MAGIC, UART_DRIVER_ABI, sizeof(struct uart_driver),
    cfuart_init, cfuart_poll, cfuart_stop, cfuart_error, cfuart_read, cfuart_consume,
    cfuart_write, cfuart_set_lines, cfuart_status
};
