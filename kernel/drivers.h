/* In-kernel device drivers. */
#ifndef GLAZE_DRIVERS_H
#define GLAZE_DRIVERS_H

#include "proc.h"

#ifndef GLAZE_BUILD
#define GLAZE_BUILD "dev"
#endif
#ifndef GLAZE_VERSION
#define GLAZE_VERSION "0"       /* the VERSION file, which OTA updates compare */
#endif

/* main.c: the companion clock gates Glaze relies on, at boot and after a
 * HIBERNATE. */
void companion_gates_on(void);

/* power.c: SYS_POWER_SLEEP (docs/hardware/POWER.md "Power off"). */
int power_sleep(int mode, uint32_t alarm_s);
bool power_trap(void *tf);              /* trap(): the wake interrupt, while asleep */
void power_boot(void);                  /* kmain: drop a stale resume record */

/* ota.c: OTA updates (docs/os/OTA.md). */
void ota_boot(void);                    /* kmain: what the SPL left (glaze/ota.h) */
int sys_ota_status(struct addrspace *as, uint32_t uout, uint32_t size);
int sys_ota_stage(struct proc *p, uint32_t op, uint32_t a1, uint32_t a2, uint32_t a3);

/* irq.c: VR4131 ICU / GIU / VRC4173 cascade. */
void irq_init(void);
void irq_dispatch_ip2(void);

/* USB device transport and the kernel-started development service. */
void usb_init(void);
void usb_poll(void);
void usb_stop(void);
void usb_resume(void);      /* after a HIBERNATE */
bool usb_tx_idle(void);
bool usb_reply_sent(void);
void usb_log_putc(char c);
void usb_panic_poll(void);
int sys_usb_ctl(struct proc *p, uint32_t op, uint32_t buf, uint32_t len);
int usb_reload_ctl(struct proc *p, uint32_t op, uint32_t buf, uint32_t len);
int usb_input_ctl(struct proc *p, uint32_t op, uint32_t buf, uint32_t len);
void usb_input_poll(void);
void usb_input_cancel(void);
void machine_reboot_ce(void) __attribute__((noreturn));
void machine_reboot(void) __attribute__((noreturn));    /* a cold restart through the ROM */
int sys_boot_mode(void);
int sys_reboot_ce(struct proc *p);

/* rtc.c */
void rtc_init(void);
int sys_rtc_get(struct addrspace *as, uint32_t uout);
int sys_rtc_set(struct addrspace *as, uint32_t uin);

/* cmcu.c: Casio's power-management microcontroller. */
void cmcu_init(void);
void cmcu_tick(void);
bool cmcu_stop(void);       /* the watchdog off for a sleep: true if it answered */
void cmcu_start(void);

/* input.c: touch panel and buttons. */
void input_init(void);
/* display.c: physical backlight and panel contrast, stock BE-300 levels. */
/* After timer_init(); inherit only for a validated GlazeBoot RAM handoff. */
void display_init(bool inherit);
/* Boot splash progress after a NAND boot (kernel/bootsplash.c). */
void splash_start(void);
bool splash_live(void);
void splash_step(unsigned permille);
void splash_tick(void);
void splash_end(void);
int display_set(int brightness, int contrast);
int display_power(int on);
void display_sleep(void);   /* panel and backlight off (drivers/power.c) */
void display_wake(void);    /* panel on, backlight still off */
void input_push(uint16_t type, uint16_t code, int x, int y);
void stowaway_init(void);
void stowaway_resume(void);
void stowaway_poll(void);
int stowaway_status(void);
void input_irq(uint16_t kj_status);
void input_poll(void);
int input_bind(int port);
void input_touch_restart(void);
void input_suspend(void);
void input_resume(bool cold);
int input_script_next_ms(void);       /* INPUT_SCRIPT builds: -1 when done */
void input_notify(uint32_t bits);     /* to the compositor's input port */

/* battery.c: battery, adapter and charger (docs/hardware/POWER.md). */
void battery_init(void);
void battery_tick(void);
int battery_status(struct addrspace *as, uint32_t uout, uint32_t size);
void battery_idle(uint32_t us);       /* a STANDBY ended after us */
void battery_suspend(void);
void battery_resume(void);
int input_read(struct addrspace *as, uint32_t uevents, uint32_t max);

/* flash.c: raw NAND for the file server. */
int sys_flash_read(struct proc *p, uint32_t page, uint32_t ubuf);
int sys_flash_prog(struct proc *p, uint32_t page, uint32_t ubuf);
int sys_flash_erase(struct proc *p, uint32_t block);
int sys_flash_bad(struct proc *p, uint32_t block);
int sys_flash_info(struct proc *p, uint32_t uout);
int sys_nand_raw_read(struct proc *p, uint32_t page, uint32_t ubuf);
int sys_nand_chip_status(struct proc *p);
int sys_nand_repair(struct proc *p, uint32_t op, uint32_t buf, uint32_t len);
int sys_nand_read(struct proc *p, uint32_t page, uint32_t ubuf);
int sys_nand_prog(struct proc *p, uint32_t page, uint32_t ubuf);
int sys_nand_erase(struct proc *p, uint32_t block);

/* hwdump.c: register snapshots for cold-boot bring-up (HWDUMP=1). */
void hwdump_setup(uint32_t *words, uint32_t nwords);
void hwdump_capture(uint32_t slot);
bool hwdump_active(void);
int sys_hwdump(struct proc *p, uint32_t op, uint32_t uaddr, uint32_t len);
void cf_debug(uint32_t out[8]);

/* cf.c: CompactFlash ATA card for the file server. */
void pcmcia_cold_init(bool force);
void cf_init(void);
void bt_insert(void);
void bt_removed(void);
void bt_poll(void);
bool bt_ready(void);
bool bt_present(void);
uint32_t bt_generation(void);
int sys_bt_io(struct proc *, uint32_t, uint32_t, uint32_t);
int sys_bt_card_info(struct proc *, uint32_t, uint32_t);
bool cf_card_is_veo(void);
void presenter_insert(uint32_t cor, int index);
void presenter_removed(void);
void presenter_poll(void);
bool presenter_active(void);
bool cf_card_is_icsi_camera(void);
void cf_probe_hold(void);
int sys_cam_probe(struct proc *, uint32_t, uint32_t, uint32_t, uint32_t);
/* modem.c: a CF data modem's UART, for the network server (PROC_NET). */
void modem_insert(uint32_t cor_address, unsigned config_index);
void modem_removed(void);
void modem_poll(void);
bool modem_ready(void);
bool modem_present(void);
uint32_t modem_generation(void);
int sys_modem_io(struct proc *, uint32_t, uint32_t, uint32_t);
/* gps.c: an NMEA receiver on a CF serial card, readable by any process. */
void gps_insert(uint32_t cor_address, unsigned config_index);
void gps_removed(void);
void gps_poll(void);
bool gps_ready(void);
uint32_t gps_generation(void);
int sys_gps_io(struct proc *, uint32_t, uint32_t, uint32_t);
int sys_cf_info(struct proc *p, uint32_t uout);
/* Looks at the PCMCIA socket and handles an insertion or removal. */
void pcmcia_poll(void);
void pcmcia_resume(void);   /* after a HIBERNATE: everything inserted afresh */
bool pcmcia_suspend(void);  /* before a sleep: a network card's socket off */
/* A card in the CF socket or the PCMCIA jacket's (socket 1), and the card
 * windows of whichever holds it (hw.h). */
bool pcmcia_card_present(void);
void pcmcia1_irq_ack(void);     /* GIRQ0 source 6: clear socket 1's cause */
void pcmcia1_irq_arm(bool driver_up);  /* from the tick: route socket 1's interrupt */
extern uint32_t pcmcia_io_base, pcmcia_attr_base;
/* The CIS's LAN_NODE_ID (CISTPL_FUNCE): a network card's station address. */
bool pcmcia_node_id(uint8_t mac[6]);
int sys_cf_read(struct proc *p, uint32_t lba, uint32_t count, uint32_t ubuf);
int sys_cf_write(struct proc *p, uint32_t lba, uint32_t count, uint32_t ubuf);

/* Network card dispatch and the shared frame API (net.c). One card owns the
 * socket until it is pulled: found at boot, or inserted later (cf.c's
 * pcmcia_poll(), which powers and configures it first). */
void net_init(void);
int net_handle(struct proc *p);
void net_irq(void);
bool net_present(void);     /* claimed: keep ATA off it even if it failed */
uint32_t net_generation(void);
uint32_t net_driver_kind(void);
uint32_t net_driver_pages(void);
bool net_up(void);          /* card driver running, or an enabled USB tunnel */
void net_usb_changed(void);
bool usbnet_up(void);
uint32_t usbnet_pages(void);
void usbnet_stop(void);     /* IRQ safe; invalidate before deferred teardown */
void usbnet_collect(void);  /* deferred reclamation after reset/service death */
int usbnet_info(struct proc *p, uint32_t out);
int usbnet_bind(int port);
int usbnet_send(struct proc *p, uint32_t buf, uint32_t len);
int usbnet_recv(struct proc *p, uint32_t buf, uint32_t max);
int usbnet_ctl(struct proc *p, uint32_t op, uint32_t buf, uint32_t len);
bool net_insert(unsigned manufacturer, unsigned product);  /* hot: true if up, or its driver is
                                                             * pending or not installed */
void net_retry(void);       /* service worker: a pending or newly installed driver */
void net_removed(void);
void net_set_mac(const uint8_t mac[6]);
void net_receive(const uint8_t *frame, uint32_t len);
void net_rx_drop(void);
void net_overrun(void);
/* A driver's power state (GZ_CARD_POWER_*), from its IRQ path; drivers that
 * never call it count as GZ_CARD_POWER_FULL while up. */
void net_power_note(unsigned state);
void net_power_status(struct gz_power_status *st);
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
bool el3_init(void);         /* 3c589.c: 3Com EtherLink III */
void el3_irq(void);
bool el3_present(void);
void el3_removed(void);
int el3_send(const uint8_t *frame, uint32_t len);
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
void audio_suspend(void);           /* DMA stopped for a sleep */
void audio_resume(bool cold);       /* cold: after a HIBERNATE */
void buzzer_init(void);
void buzzer_beeps(unsigned n);
void buzzer_poll(void);
void buzzer_stop(void);
bool buzzer_active(void);
int sys_buzzer_tone(struct proc *p, uint32_t hz, uint32_t ms, uint32_t pulse);

#endif
