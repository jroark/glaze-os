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
/* Looks at the PCMCIA socket and handles an insertion or removal. */
void pcmcia_poll(void);
/* The CIS's LAN_NODE_ID (CISTPL_FUNCE): a network card's station address. */
bool pcmcia_node_id(uint8_t mac[6]);
int sys_cf_read(struct proc *p, uint32_t lba, uint32_t count, uint32_t ubuf);
int sys_cf_write(struct proc *p, uint32_t lba, uint32_t count, uint32_t ubuf);

/* Network card dispatch and the shared frame API (net.c). One card owns the
 * socket until it is pulled: found at boot, or inserted later (cf.c's
 * pcmcia_poll(), which powers and configures it first). */
void net_init(void);
void net_irq(void);
bool net_present(void);     /* claimed: keep ATA off it even if it failed */
bool net_up(void);          /* claimed and its driver running */
bool net_insert(unsigned manufacturer, unsigned product);  /* hot: true if up */
void net_removed(void);
void net_set_mac(const uint8_t mac[6]);
void net_receive(const uint8_t *frame, uint32_t len);
void net_rx_drop(void);
void net_overrun(void);
int sys_wifi_ctl(struct proc *p, uint32_t op, uint32_t buf);
int sys_net_info(struct proc *p, uint32_t uout);
int sys_net_bind(struct proc *p, int port);
int sys_net_send(struct proc *p, uint32_t ubuf, uint32_t len);
int sys_net_recv(struct proc *p, uint32_t ubuf, uint32_t max);
/* Backends: identified by the CIS before their registers are touched. The
 * NE2000 and WL54-CF move frames through net.c's queue; the XI-825 keeps its
 * own (xi825.c). */
void ne2000_init(void);
void ne2000_irq(void);
bool ne2000_present(void);
void ne2000_removed(void);
int ne2000_send(const uint8_t *frame, uint32_t len);
bool wl54cf_init(void);
void wl54cf_irq(void);
void wl54cf_removed(void);
int wl54cf_send(const uint8_t *frame, uint32_t len);
int wl54cf_control(uint32_t op, void *buf);   /* Wi-Fi controls (kernel buffer) */
void xi825_init(void);
void xi825_removed(void);
void xi825_irq(void);
bool xi825_present(void);
int xi825_info(struct proc *p, uint32_t out);
int xi825_bind(struct proc *p, int port);
int xi825_send(struct proc *p, uint32_t buf, uint32_t len);
int xi825_recv(struct proc *p, uint32_t buf, uint32_t len);
int xi825_control(struct proc *p, uint32_t op, uint32_t buf);

/* audio.c: wave-output DMA for the audio server. */
void audio_init(void);
void audio_irq(void);
int sys_audio_bind(struct proc *p, int port);
int sys_audio_start(struct proc *p, uint32_t rate, uint32_t channels);
int sys_audio_write(struct proc *p, uint32_t ubuf, uint32_t len);
int sys_audio_stop(struct proc *p, uint32_t flush);
int sys_audio_status(struct proc *p, uint32_t uout);
int sys_audio_mix(struct proc *p, uint32_t ubuf, uint32_t len);
int sys_audio_gain(struct proc *p, uint32_t gain);

#endif
