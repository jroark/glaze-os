#ifndef GLAZE_STDIO_H
#define GLAZE_STDIO_H

#include <stdarg.h>
#include <stddef.h>

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int puts(const char *s);

#endif
