/* Deterministic system calls for native Settings rendering tests. */
#ifndef SETTINGS_TEST_SYS_H
#define SETTINGS_TEST_SYS_H
#define GLAZE_SYS_H
#include <glaze/abi.h>
uint32_t gz_time_ms(void);
void gz_sleep_ms(uint32_t ms);
int gz_rtc_get(struct gz_datetime *dt);
int gz_rtc_set(const struct gz_datetime *dt);
int gz_sysinfo(struct gz_sysinfo *si);
int gz_boot_mode(void);
int gz_power_status(struct gz_power_status *st);
int gz_port_lookup(const char *name);
int gz_notify(int port, uint32_t bits);
int gz_getpid(void);
int gz_send(int port, const struct gz_msg *msg, struct gz_msg *reply);
#endif
