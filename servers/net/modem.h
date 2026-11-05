/* Dial-up networking over a PC Card modem (modem.c): AT dialling, then PPP. */
#ifndef GLAZE_NET_MODEM_H
#define GLAZE_NET_MODEM_H
#include <glaze/net.h>

void modem_attach(void);        /* the kernel reports a GZ_NET_MODEM device */
void modem_detach(void);
void modem_poll(void);          /* the serial stream, the dialer, PPP state */
int modem_request(int op, void *buf);   /* NET_DIALUP_*, the session buffer */
void modem_fill_status(struct net_status *st);

#endif
