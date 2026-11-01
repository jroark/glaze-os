#ifndef UI_EVENTS_TEST_SYS_H
#define UI_EVENTS_TEST_SYS_H
/* The syscalls lib/ui/app.c makes, provided by tests/ui_events.c. */
#define GLAZE_SYS_H
#include <glaze/abi.h>
int gz_port_create(void);
int gz_port_lookup(const char *name);
int gz_send(int port, const struct gz_msg *m, struct gz_msg *r);
int gz_recv(int port, struct gz_msg *m, int timeout_ms);
void *gz_shm_map(int id);
int gz_getpid(void);
void gz_sleep_ms(uint32_t ms);
uint32_t gz_time_ms(void);
int gz_proc_set_bg(int pid, int state);
#endif
