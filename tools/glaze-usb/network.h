#ifndef HOST_DEBUG_NETWORK_H
#define HOST_DEBUG_NETWORK_H
#include "transport.h"
int debug_host_open(struct usb_link *,const char *,const char *);
void debug_host_close(struct usb_link *);
int debug_host_read(struct usb_link *,void *,size_t,unsigned);
int debug_host_write(struct usb_link *,const void *,size_t,unsigned);
int debug_host_keygen(const char *);
int debug_host_key_valid(const char *);
#endif
