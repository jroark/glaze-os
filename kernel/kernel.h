#ifndef GLAZE_KERNEL_H
#define GLAZE_KERNEL_H

#include "types.h"

#include <stdarg.h>

/* console.c */
void uart_putc(char c);
void console_use_debug_uart(void);
void console_ram_boot(void);
bool console_dead(void);
bool timer_rtc_ok(void);
void kputs(const char *s);
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, va_list ap);
void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

#define KASSERT(c) \
    do { if (!(c)) panic("assert %s at %s:%d", #c, __FILE__, __LINE__); } while (0)

#include <string.h>

#endif
