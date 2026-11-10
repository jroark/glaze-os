/* Trusted romfs kernel modules, ELF32/MIPS O32 relocatable objects. */
#ifndef GLAZE_MODULE_H
#define GLAZE_MODULE_H
#include "proc.h"

struct module {
    void *base, *entry;
    uint32_t pages, bytes, text_bytes;
};
int module_load(const char *path, const char *symbol, struct module *module);
void module_unload(struct module *module);
bool module_executable(const struct module *module, const void *p);
bool module_contains(const struct module *module, const void *p, uint32_t size);

/* CF serial cards (Bluetooth BCSP, data modems): one 16550 behind the socket's
 * I/O window. The socket dispatcher (cf.c) picks the configuration from the CIS. */
#define UART_DRIVER_MAGIC 0x47554152u
#define UART_DRIVER_ABI 2u
struct uart_config {
    uint32_t cor_address;   /* attribute offset of the COR (CONFIG tuple) */
    uint16_t io_base;       /* ISA port the window decodes, e.g. 0x3f8 */
    uint16_t divisor;       /* 16550 DLL/DLM */
    uint8_t cor, lcr, mcr, fcr;
    /* RTS/CTS flow control (modems): RTS drops while the receive ring is
     * nearly full, and nothing is sent while CTS is low. */
    uint8_t rts_cts;
};
struct uart_driver {
    uint32_t magic, abi, size;
    int (*init)(const struct uart_config *);
    void (*poll)(void);
    void (*stop)(void);
    int (*error)(void);
    int (*read)(uint8_t *, uint32_t); /* peek: consume only after a successful user copy */
    void (*consume)(uint32_t);
    int (*write)(const uint8_t *, uint32_t);
    void (*set_lines)(uint8_t mcr);   /* DTR bit 0, RTS bit 1; OUT2 is kept */
    uint8_t (*status)(void);          /* 16550 MSR: CTS 0x10, DSR 0x20, RI 0x40, DCD 0x80 */
};
#define NET_DRIVER_MAGIC 0x474e4452u
#define NET_DRIVER_ABI 1u
/* Built together with the kernel; these callbacks are not a userspace ABI. */
struct net_driver {
    uint32_t magic, abi, size, kind;
    bool (*init)(void);
    bool (*present)(void);
    void (*irq)(void);
    void (*stop)(void);
    int (*send_frame)(const uint8_t *, uint32_t);
    int (*control)(uint32_t, void *);
    int (*info)(struct proc *, uint32_t);
    int (*bind)(struct proc *, int);
    int (*send)(struct proc *, uint32_t, uint32_t);
    int (*recv)(struct proc *, uint32_t, uint32_t);
    int (*wifi)(struct proc *, uint32_t, uint32_t);
};
#endif
