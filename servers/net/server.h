/* Network server internals shared between main.c and netif.c. */
#ifndef GLAZE_NET_SERVER_H
#define GLAZE_NET_SERVER_H

#include <glaze/net.h>

void net_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* netif.c */
int net_if_init(int port);
void net_if_poll(void);         /* frames from the kernel, DHCP fallback */
int net_if_state(void);
void net_if_status(struct net_status *st);

#endif
