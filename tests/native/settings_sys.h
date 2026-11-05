/* Deterministic system calls for native Settings rendering tests. */
#ifndef SETTINGS_TEST_SYS_H
#define SETTINGS_TEST_SYS_H
#define GLAZE_SYS_H
#include <glaze/abi.h>
#include <glaze/bluetooth.h>
int gz_bt_card_info(struct bt_card_info *, uint32_t);
uint32_t gz_time_ms(void);
void gz_sleep_ms(uint32_t ms);
int gz_rtc_get(struct gz_datetime *dt);
int gz_rtc_set(const struct gz_datetime *dt);
int gz_sysinfo(struct gz_sysinfo *si);
#include <glaze/ota.h>
int gz_ota_status(struct gz_ota_status *st);
int gz_boot_mode(void);
int gz_power_status(struct gz_power_status *st);
int gz_port_lookup(const char *name);
int gz_notify(int port, uint32_t bits);
int gz_getpid(void);
void *gz_shm_map(int);
int gz_shm_unmap(void *);
int gz_send(int port, const struct gz_msg *msg, struct gz_msg *reply);
const void *gz_romfs_map(const char *name, uint32_t *size);
#endif
