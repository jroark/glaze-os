/* In-kernel device drivers. */
#ifndef GLAZE_DRIVERS_H
#define GLAZE_DRIVERS_H

#include "proc.h"

#ifndef GLAZE_BUILD
#define GLAZE_BUILD "dev"
/* ne2000.c: PCMCIA Ethernet for the network server. */
void ne2000_init(void);
void ne2000_irq(void);
int sys_net_info(struct proc *p, uint32_t uout);
int sys_net_bind(struct proc *p, int port);
int sys_net_send(struct proc *p, uint32_t ubuf, uint32_t len);
int sys_net_recv(struct proc *p, uint32_t ubuf, uint32_t max);

/* audio.c: wave-output DMA for the audio server. */
void audio_init(void);
void audio_irq(void);
int sys_audio_bind(struct proc *p, int port);
int sys_audio_start(struct proc *p, uint32_t rate, uint32_t channels);
int sys_audio_write(struct proc *p, uint32_t ubuf, uint32_t len);
int sys_audio_stop(struct proc *p, uint32_t flush);
int sys_audio_status(struct proc *p, uint32_t uout);

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
void input_push(uint16_t type, uint16_t code, int x, int y);
void stowaway_init(void);
void stowaway_poll(void);
int stowaway_status(void);
void input_irq(uint16_t kj_status);
void input_poll(void);
int input_bind(int port);
int input_read(struct addrspace *as, uint32_t uevents, uint32_t max);

/* flash.c: raw NAND for the file server. */
int sys_flash_read(struct proc *p, uint32_t page, uint32_t ubuf);
int sys_flash_prog(struct proc *p, uint32_t page, uint32_t ubuf);
int sys_flash_erase(struct proc *p, uint32_t block);

/* ne2000.c: PCMCIA Ethernet for the network server. */
void ne2000_init(void);
void ne2000_irq(void);
int sys_net_info(struct proc *p, uint32_t uout);
int sys_net_bind(struct proc *p, int port);
int sys_net_send(struct proc *p, uint32_t ubuf, uint32_t len);
int sys_net_recv(struct proc *p, uint32_t ubuf, uint32_t max);

/* audio.c: wave-output DMA for the audio server. */
void audio_init(void);
void audio_irq(void);
int sys_audio_bind(struct proc *p, int port);
int sys_audio_start(struct proc *p, uint32_t rate, uint32_t channels);
int sys_audio_write(struct proc *p, uint32_t ubuf, uint32_t len);
int sys_audio_stop(struct proc *p, uint32_t flush);
int sys_audio_status(struct proc *p, uint32_t uout);

#endif
