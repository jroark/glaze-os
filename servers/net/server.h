/* Network server internals shared between main.c and netif.c. */
#ifndef GLAZE_NET_SERVER_H
#define GLAZE_NET_SERVER_H

#include <glaze/net.h>

void net_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* netif.c */
int net_if_init(int port);
int net_if_wifi(uint32_t op, void *buf);
void net_if_poll(void);         /* frames from the kernel, DHCP fallback */
int net_if_state(void);
void net_if_status(struct net_status *st);
void net_if_power(bool screen_on);  /* NET_POWER */

/* modem.c: dial-up PPP over a CF modem, when there is no network card. */
int net_modem_init(int port);
bool net_modem_present(void);
void net_modem_poll(void);
int net_modem_request(uint32_t op, void *buf);   /* NET_DIAL ... NET_DIAL_CONFIG */
int net_modem_state(void);
void net_modem_status(struct net_status *st);

#endif
