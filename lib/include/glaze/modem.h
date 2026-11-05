/*
 * A PC Card modem (FUNCID serial, a 16550 UART), as the network server
 * sees it: a network device of kind GZ_NET_MODEM whose SYS_NET_SEND and
 * SYS_NET_RECV carry the serial byte stream instead of Ethernet frames,
 * and whose SYS_WIFI_CTL takes the operations below. Only PROC_NET may use
 * them. The kernel driver is kernel/drivers/modem.c.
 */
#ifndef GLAZE_MODEM_H
#define GLAZE_MODEM_H
#include <stdint.h>

#define MODEM_STATUS    0x100   /* argument: struct modem_status * */
#define MODEM_DTR       0x101   /* argument: 0 drops DTR (hang up), else raises it */
#define MODEM_BAUD      0x102   /* argument: 300..115200, a divisor of 115200 */

/* 16550 modem status register bits (MSR), as reported in msr. */
#define MODEM_CTS       0x10
#define MODEM_DSR       0x20
#define MODEM_RI        0x40
#define MODEM_DCD       0x80

#define MODEM_STATUS_VERSION 1
struct modem_status {
    uint32_t version;           /* MODEM_STATUS_VERSION */
    uint32_t msr;               /* MODEM_CTS ... MODEM_DCD */
    uint32_t baud;
    uint32_t dtr;               /* 1 while DTR is raised */
    uint32_t rx_bytes, tx_bytes;
    uint32_t rx_overruns;       /* UART overruns and ring overflows */
    uint32_t port;              /* the card's I/O base */
    char identification[48];    /* the card's VERS_1 strings */
};

#endif
