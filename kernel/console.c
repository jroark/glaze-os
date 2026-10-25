/* Polled UART console on the VRC4173 SIU (emulator stdout). */

#include "kernel.h"
#include "hw.h"
#include <glaze/bootinfo.h>

#include <stdarg.h>

static bool debug_uart;
static bool dead;               /* the UART never became ready: output off */
static bool tracing;            /* a RAM boot: panics also paint the trace */

/* Polls for THRE before giving up on the UART for good. After Windows CE
 * (a RAM boot) the companion SIU may be gated or unpowered, and an endless
 * wait would hang the kernel before it draws anything. Generous: a byte
 * takes about 1 ms at 9600 baud. */
#define THRE_POLLS          200000u

/* A RAM boot leaves the UART as Windows CE set it up: GlazeBoot opens COM1:
 * at 115200 8N1 and leaves it open, which powers the sync cable's
 * transceiver (as CyaCE did for linux4be, whose serial_be300.c also
 * programmed nothing). Only note that a panic should paint the trace too. */
void console_ram_boot(void)
{
    tracing = true;
}

bool console_dead(void)
{
    return dead;
}

void console_use_debug_uart(void)
{
    /* VR4131 UM sections 18.2.2/18.2.10: DSIUTH/DSIULS at
     * PA 0x0F000820/0x0F000825; NK OAL 0x80078308 uses this TX path.
     * Use its independent TX path when the dock owns the companion SIU. */
    debug_uart = true;
}

void uart_putc(char c)
{
    uint32_t lsr = debug_uart ? 0x0F000825u : UART_BASE + 5 * 4;
    uint32_t thr = debug_uart ? 0x0F000820u : UART_BASE;
    uint32_t n = 0;

    if (dead)
        return;
    while (!(REG8(lsr) & UART_LSR_THRE))
        if (++n == THRE_POLLS) {
            dead = true;
            return;
        }
    REG8(thr) = (uint8_t)c;
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
    /* The boot trace's third row all red (glaze/bootinfo.h). */
    for (uint32_t n = 24; tracing && n < 36; n++)
        gz_boot_trace(n, GZ_TRACE_RED);
    for (;;)
        ;
}
