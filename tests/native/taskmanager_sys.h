#ifndef TASKMANAGER_TEST_SYS_H
#define TASKMANAGER_TEST_SYS_H
#define GLAZE_SYS_H
#include <glaze/abi.h>
uint32_t gz_time_ms(void);
int gz_task_snapshot(struct gz_task_snapshot *s);
int gz_sysinfo(struct gz_sysinfo *s);
int gz_task_stop(int pid);
#endif
