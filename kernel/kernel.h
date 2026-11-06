#ifndef GLAZE_KERNEL_H
#define GLAZE_KERNEL_H

#include "types.h"

#include <stdarg.h>

struct gz_nand_manifest;
void boot_nand_read(uint32_t page, void *buf);
const struct gz_nand_manifest *boot_nand_manifest(void);
struct nand_boot_map;
const struct nand_boot_map *boot_nand_map(void);   /* the scan boot_nand_read uses */

/* console.c */
void uart_putc(char c);
void console_use_debug_uart(bool on);
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
