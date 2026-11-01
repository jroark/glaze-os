#ifndef SOMAFM_TEST_SYS_H
#define SOMAFM_TEST_SYS_H
#define GLAZE_SYS_H
#include <glaze/abi.h>
uint32_t gz_time_ms(void);
int gz_log(const char *s, uint32_t n);
int gz_sysinfo(struct gz_sysinfo *si);
#endif
