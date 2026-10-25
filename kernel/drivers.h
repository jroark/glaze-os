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

/* cmcu.c: Casio's power-management microcontroller. */
void cmcu_init(void);
void cmcu_tick(void);

/* input.c: touch panel and buttons. */
void input_init(void);
/* display.c: physical backlight and panel contrast, stock BE-300 levels. */
int display_set(int brightness, int contrast);
int display_power(int on);
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
int sys_flash_info(struct proc *p, uint32_t uout);
int sys_nand_read(struct proc *p, uint32_t page, uint32_t ubuf);
int sys_nand_prog(struct proc *p, uint32_t page, uint32_t ubuf);
int sys_nand_erase(struct proc *p, uint32_t block);

/* cf.c: CompactFlash ATA card for the file server. */
void cf_init(void);
int sys_cf_info(struct proc *p, uint32_t uout);
int sys_cf_read(struct proc *p, uint32_t lba, uint32_t count, uint32_t ubuf);
int sys_cf_write(struct proc *p, uint32_t lba, uint32_t count, uint32_t ubuf);

/* net.c: select one boot-time card and dispatch privileged raw frame I/O. */
void netdev_init(void);
void netdev_irq(void);
bool netdev_present(void);
bool netdev_claimed(void); /* keep ATA off a detected NIC even if its init failed */
int sys_wifi_ctl(struct proc *p, uint32_t op, uint32_t buf);
/* ne2000.c and xi825.c: hardware backends. */
void ne2000_init(void);
void ne2000_irq(void);
bool ne2000_present(void);
void xi825_init(void);
void xi825_irq(void);
bool xi825_present(void);
int xi825_info(struct proc *p, uint32_t out);
int xi825_bind(struct proc *p, int port);
int xi825_send(struct proc *p, uint32_t buf, uint32_t len);
int xi825_recv(struct proc *p, uint32_t buf, uint32_t len);
int xi825_control(struct proc *p, uint32_t op, uint32_t buf);
int ne2000_info(struct proc *p, uint32_t out);
int ne2000_bind(struct proc *p, int port);
int ne2000_send(struct proc *p, uint32_t buf, uint32_t len);
int ne2000_recv(struct proc *p, uint32_t buf, uint32_t len);
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
int sys_audio_mix(struct proc *p, uint32_t ubuf, uint32_t len);

#endif
