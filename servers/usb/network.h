#ifndef DEBUG_NETWORK_H
#define DEBUG_NETWORK_H
#include <stdint.h>
/* Nonblocking poll: 1 = authenticated, 0 = idle/handshake, -1 = lost session.
 * A USB session prevents accepting another debugger. */
int debug_net_poll(int busy);
int debug_net_read(void *, unsigned);
int debug_net_write(const void *, unsigned);
int debug_net_active(void);
void debug_net_drop(void);
void debug_net_restore(void);
int debug_net_handoff(void);
#endif
