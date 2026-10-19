/* Polled UART console on the VRC4173 SIU (emulator stdout). */

#include "kernel.h"
#include "hw.h"

#include <stdarg.h>

void uart_putc(char c)
{
    while (!(UART_LSR & UART_LSR_THRE))
        ;
    UART_THR = (uint8_t)c;
}

void kputs(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

static void put_num(uint32_t v, unsigned base, int width, char pad, bool neg)
{
    static const char digits[] = "0123456789abcdef";
    char buf[12];
    int n = 0;

    do {
        buf[n++] = digits[v % base];
        v /= base;
    } while (v);
    if (neg)
        buf[n++] = '-';
    while (n < width)
        buf[n++] = pad;
    while (n)
        uart_putc(buf[--n]);
}

void kvprintf(const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            if (*fmt == '\n')
                uart_putc('\r');
            uart_putc(*fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        switch (*fmt) {
        case 'd': {
            int v = va_arg(ap, int);
            put_num(v < 0 ? -(uint32_t)v : (uint32_t)v, 10, width, pad, v < 0);
            break;
        }
        case 'u':
            put_num(va_arg(ap, uint32_t), 10, width, pad, false);
            break;
        case 'x':
            put_num(va_arg(ap, uint32_t), 16, width, pad, false);
            break;
        case 'p':
            kputs("0x");
            put_num(va_arg(ap, uint32_t), 16, 8, '0', false);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            kputs(s ? s : "(null)");
            break;
        }
        case 'c':
            uart_putc((char)va_arg(ap, int));
            break;
        case '%':
            uart_putc('%');
            break;
        default:
            uart_putc('?');
            break;
        }
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

void panic(const char *fmt, ...)
{
    va_list ap;
    kputs("\n*** PANIC: ");
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kputs("\n");
    for (;;)
        ;
}
