/* Stands in for glaze/sys.h when lib/libc/printf.c is built natively
 * (host/Makefile libc-printf-test): the test supplies gz_log. */
#ifndef GLAZE_SYS_H
#define GLAZE_SYS_H
#include <stdbool.h>
#include <stdint.h>
int gz_log(const char *s, uint32_t len);
#endif
