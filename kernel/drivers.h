/* In-kernel device drivers. */
#ifndef GLAZE_DRIVERS_H
#define GLAZE_DRIVERS_H

#include "proc.h"

#ifndef GLAZE_BUILD
#define GLAZE_BUILD "dev"
#endif

/* irq.c: VR4131 ICU / GIU / VRC4173 cascade. */
void irq_init(void);
void irq_dispatch_ip2(void);

/* rtc.c */
void rtc_init(void);
int sys_rtc_get(struct addrspace *as, uint32_t uout);
int sys_rtc_set(struct addrspace *as, uint32_t uin);

/* input.c: touch panel and buttons. */
void input_init(void);
void input_irq(uint16_t kj_status);
void input_poll(void);
int input_bind(int port);
int input_read(struct addrspace *as, uint32_t uevents, uint32_t max);

/* kv.c: NAND-backed key/value store. */
void kv_init(void);
int sys_kv_get(struct addrspace *as, uint32_t ukey, uint32_t ubuf, uint32_t len);
int sys_kv_put(struct addrspace *as, uint32_t ukey, uint32_t ubuf, uint32_t len);

#endif
