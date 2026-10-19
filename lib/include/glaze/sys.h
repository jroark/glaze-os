/* Userspace syscall wrappers. */
#ifndef GLAZE_SYS_H
#define GLAZE_SYS_H

#include <glaze/abi.h>

static inline int __gz_sc(int n, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    register uint32_t v0 __asm__("v0") = (uint32_t)n;
    register uint32_t a0 __asm__("a0") = a;
    register uint32_t a1 __asm__("a1") = b;
    register uint32_t a2 __asm__("a2") = c;
    register uint32_t a3 __asm__("a3") = d;

    /* The kernel restores every register except v0. */
    __asm__ volatile("syscall" : "+r"(v0) : "r"(a0), "r"(a1), "r"(a2), "r"(a3) : "memory");
    return (int)v0;
}

#define GZ_SC0(n)           __gz_sc((n), 0, 0, 0, 0)
#define GZ_SC1(n, a)        __gz_sc((n), (uint32_t)(a), 0, 0, 0)
#define GZ_SC2(n, a, b)     __gz_sc((n), (uint32_t)(a), (uint32_t)(b), 0, 0)
#define GZ_SC3(n, a, b, c)  __gz_sc((n), (uint32_t)(a), (uint32_t)(b), (uint32_t)(c), 0)

#define GZ_SPAWN_PRIVILEGED 0x1

static inline void gz_exit(int code) { GZ_SC1(SYS_EXIT, code); for (;;) ; }
static inline void gz_yield(void) { GZ_SC0(SYS_YIELD); }
static inline void gz_sleep_ms(uint32_t ms) { GZ_SC1(SYS_SLEEP_MS, ms); }
static inline int gz_log(const char *s, uint32_t len) { return GZ_SC2(SYS_LOG, s, len); }
static inline int gz_spawn(const char *path, uint32_t flags) { return GZ_SC2(SYS_SPAWN, path, flags); }
static inline int gz_kill(int pid) { return GZ_SC1(SYS_KILL, pid); }
static inline int gz_getpid(void) { return GZ_SC0(SYS_GETPID); }
static inline int gz_proc_alive(int pid) { return GZ_SC1(SYS_PROC_ALIVE, pid); }

static inline int gz_port_create(void) { return GZ_SC0(SYS_PORT_CREATE); }
static inline int gz_port_register(int port, const char *name) { return GZ_SC2(SYS_PORT_REGISTER, port, name); }
static inline int gz_port_lookup(const char *name) { return GZ_SC1(SYS_PORT_LOOKUP, name); }
static inline int gz_send(int port, const struct gz_msg *m, struct gz_msg *reply) { return GZ_SC3(SYS_SEND, port, m, reply); }
static inline int gz_recv(int port, struct gz_msg *m, int timeout_ms) { return GZ_SC3(SYS_RECV, port, m, timeout_ms); }
static inline int gz_reply(int token, const struct gz_msg *m) { return GZ_SC2(SYS_REPLY, token, m); }
static inline int gz_notify(int port, uint32_t bits) { return GZ_SC2(SYS_NOTIFY, port, bits); }

static inline void *gz_mmap_anon(uint32_t len)
{
    int r = GZ_SC1(SYS_MMAP_ANON, len);
    return r < 0 ? NULL : (void *)r;
}
static inline int gz_shm_create(uint32_t len) { return GZ_SC1(SYS_SHM_CREATE, len); }
static inline void *gz_shm_map(int id)
{
    int r = GZ_SC1(SYS_SHM_MAP, id);
    return r < 0 ? NULL : (void *)r;
}

/* Milliseconds since boot (wraps after ~49 days); gz_time_us fills a u64. */
static inline uint32_t gz_time_ms(void) { return (uint32_t)GZ_SC1(SYS_TIME_US, 0); }
static inline uint64_t gz_time_us(void) { uint64_t t = 0; GZ_SC1(SYS_TIME_US, &t); return t; }
static inline int gz_rtc_get(struct gz_datetime *dt) { return GZ_SC1(SYS_RTC_GET, dt); }
static inline int gz_rtc_set(const struct gz_datetime *dt) { return GZ_SC1(SYS_RTC_SET, dt); }

static inline uint16_t *gz_fb_map(void)
{
    int r = GZ_SC0(SYS_FB_MAP);
    return r < 0 ? NULL : (uint16_t *)r;
}
static inline int gz_input_bind(int port) { return GZ_SC1(SYS_INPUT_BIND, port); }
static inline int gz_input_read(struct gz_input_event *ev, uint32_t max) { return GZ_SC2(SYS_INPUT_READ, ev, max); }

/* File server only (see servers/fs). */
static inline int gz_flash_read(uint32_t page, void *buf) { return GZ_SC2(SYS_FLASH_READ, page, buf); }
static inline int gz_flash_prog(uint32_t page, const void *buf) { return GZ_SC2(SYS_FLASH_PROG, page, buf); }
static inline int gz_flash_erase(uint32_t block) { return GZ_SC1(SYS_FLASH_ERASE, block); }

static inline int gz_kv_get(const char *key, void *buf, uint32_t len) { return GZ_SC3(SYS_KV_GET, key, buf, len); }
static inline int gz_kv_put(const char *key, const void *buf, uint32_t len) { return GZ_SC3(SYS_KV_PUT, key, buf, len); }
static inline int gz_sysinfo(struct gz_sysinfo *si) { return GZ_SC1(SYS_SYSINFO, si); }

static inline const void *gz_romfs_map(const char *name, uint32_t *size)
{
    int r = GZ_SC2(SYS_ROMFS_MAP, name, size);
    return r < 0 ? NULL : (const void *)r;
}

#endif
