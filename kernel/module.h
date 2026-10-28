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
