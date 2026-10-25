/* Userspace syscall wrappers. */
#ifndef GLAZE_SYS_H
#define GLAZE_SYS_H

#include <glaze/abi.h>
#include <glaze/loader.h>

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
/* Only privileged spawners may mark a romfs program as a managed app. */
#define GZ_SPAWN_APP 0x10

static inline void gz_exit(int code) { GZ_SC1(SYS_EXIT, code); for (;;) ; }
static inline void gz_yield(void) { GZ_SC0(SYS_YIELD); }
static inline void gz_sleep_ms(uint32_t ms) { GZ_SC1(SYS_SLEEP_MS, ms); }
static inline int gz_log(const char *s, uint32_t len) { return GZ_SC2(SYS_LOG, s, len); }
static inline int gz_spawn(const char *path, uint32_t flags) { return GZ_SC2(SYS_SPAWN, path, flags); }
/* Spawns an ELF held in memory (an installed app), always unprivileged; the
 * kernel copies it, so the buffer may be freed at once. Returns the pid. */
static inline int gz_spawn_image(const void *elf, uint32_t size, const char *name)
{ return GZ_SC3(SYS_SPAWN_IMAGE, elf, size, name); }
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
static inline int gz_munmap_anon(void *va) { return GZ_SC1(SYS_MUNMAP_ANON, va); }
static inline int gz_shm_create(uint32_t len) { return GZ_SC1(SYS_SHM_CREATE, len); }
static inline void *gz_shm_map(int id)
{
    int r = GZ_SC1(SYS_SHM_MAP, id);
    return r < 0 ? NULL : (void *)r;
}
/* The object is freed once no process maps it (its id is then reused). */
static inline int gz_shm_unmap(void *va) { return GZ_SC1(SYS_SHM_UNMAP, va); }

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
static inline int gz_input_status(void) { return GZ_SC0(SYS_INPUT_STATUS); }
static inline int gz_display_set(int brightness, int contrast) { return GZ_SC2(SYS_DISPLAY_SET, brightness, contrast); }
static inline int gz_display_power(bool on) { return GZ_SC1(SYS_DISPLAY_POWER, on); }
static inline int gz_input_bind(int port) { return GZ_SC1(SYS_INPUT_BIND, port); }
static inline int gz_input_read(struct gz_input_event *ev, uint32_t max) { return GZ_SC2(SYS_INPUT_READ, ev, max); }

/* File server only (see servers/fs). */
static inline int gz_flash_read(uint32_t page, void *buf) { return GZ_SC2(SYS_FLASH_READ, page, buf); }
static inline int gz_flash_prog(uint32_t page, const void *buf) { return GZ_SC2(SYS_FLASH_PROG, page, buf); }
static inline int gz_flash_erase(uint32_t block) { return GZ_SC1(SYS_FLASH_ERASE, block); }
static inline int gz_flash_info(struct gz_flash_info *info) { return GZ_SC1(SYS_FLASH_INFO, info); }
/* The whole NAND, in a RAM boot (glaze/bootinfo.h); pages are 512 bytes. */
static inline int gz_nand_read(uint32_t page, void *buf) { return GZ_SC2(SYS_NAND_READ, page, buf); }
static inline int gz_nand_prog(uint32_t page, const void *buf) { return GZ_SC2(SYS_NAND_PROG, page, buf); }
static inline int gz_nand_erase(uint32_t block) { return GZ_SC1(SYS_NAND_ERASE, block); }

/* CompactFlash, file server only. lba and count are in GZ_CF_SECTOR units,
 * at most GZ_CF_IO_MAX bytes per call. */
static inline int gz_cf_info(struct gz_cf_info *info) { return GZ_SC1(SYS_CF_INFO, info); }
static inline int gz_cf_read(uint32_t lba, uint32_t count, void *buf) { return GZ_SC3(SYS_CF_READ, lba, count, buf); }
static inline int gz_cf_write(uint32_t lba, uint32_t count, const void *buf) { return GZ_SC3(SYS_CF_WRITE, lba, count, buf); }

/* Network server only (see servers/net). */
static inline int gz_wifi_ctl(uint32_t op, void *buf) { return GZ_SC2(SYS_WIFI_CTL, op, buf); }
static inline int gz_net_info(struct gz_netif_info *info) { return GZ_SC1(SYS_NET_INFO, info); }
static inline int gz_net_bind(int port) { return GZ_SC1(SYS_NET_BIND, port); }
static inline int gz_net_send(const void *frame, uint32_t len) { return GZ_SC2(SYS_NET_SEND, frame, len); }
static inline int gz_net_recv(void *frame, uint32_t max) { return GZ_SC2(SYS_NET_RECV, frame, max); }

static inline int gz_image_map(const void *source, uint32_t size, uint32_t flags, struct gz_image_info *out) {
    return __gz_sc(SYS_IMAGE_MAP, (uint32_t)source, size, flags, (uint32_t)out);
}
static inline int gz_image_release(uint32_t base) { return GZ_SC1(SYS_IMAGE_RELEASE, base); }
static inline int gz_image_seal(uint32_t base) { return GZ_SC1(SYS_IMAGE_SEAL, base); }
static inline int gz_image_query(const void *addr, struct gz_image_page *out) { return GZ_SC2(SYS_IMAGE_QUERY, addr, out); }
static inline int gz_image_stats(struct gz_image_stats *out) { return GZ_SC1(SYS_IMAGE_STATS, out); }

/* Audio server only (see servers/audio). gz_audio_write returns the bytes
 * accepted; gz_audio_stop with flush 0 keeps queued PCM for a resume.
 * gz_audio_mix queues effects (GZ_AUDIO_MIX_RING) in the output's format. */
static inline int gz_audio_bind(int port) { return GZ_SC1(SYS_AUDIO_BIND, port); }
static inline int gz_audio_start(uint32_t rate, uint32_t channels) { return GZ_SC2(SYS_AUDIO_START, rate, channels); }
static inline int gz_audio_write(const void *pcm, uint32_t len) { return GZ_SC2(SYS_AUDIO_WRITE, pcm, len); }
static inline int gz_audio_stop(int flush) { return GZ_SC1(SYS_AUDIO_STOP, flush); }
static inline int gz_audio_status(struct gz_audio_status *st) { return GZ_SC1(SYS_AUDIO_STATUS, st); }
static inline int gz_audio_mix(const void *pcm, uint32_t len) { return GZ_SC2(SYS_AUDIO_MIX, pcm, len); }

/* Threads and semaphores; lib/include/glaze/thread.h has the libc layer. */
static inline int gz_thread_spawn(void (*entry)(void *), void *arg) { return GZ_SC2(SYS_THREAD_CREATE, entry, arg); }
static inline void gz_thread_exit(void) { GZ_SC0(SYS_THREAD_EXIT); for (;;) ; }
static inline int gz_gettid(void) { return GZ_SC0(SYS_GETTID); }
static inline int gz_sem_create(int count) { return GZ_SC1(SYS_SEM_CREATE, count); }
/* 0, or GZ_ETIMEDOUT (timeout_ms 0 = try, -1 = forever). */
static inline int gz_sem_wait(int sem, int timeout_ms) { return GZ_SC2(SYS_SEM_WAIT, sem, timeout_ms); }
static inline int gz_sem_post(int sem) { return GZ_SC1(SYS_SEM_POST, sem); }
static inline int gz_sem_destroy(int sem) { return GZ_SC1(SYS_SEM_DESTROY, sem); }

static inline int gz_sysinfo(struct gz_sysinfo *si) { return GZ_SC1(SYS_SYSINFO, si); }
static inline int gz_task_snapshot(struct gz_task_snapshot *s)
{ return GZ_SC2(SYS_TASK_SNAPSHOT, s, sizeof(*s)); }
/* Cannot stop the caller or system services. PIDs include a generation. */
static inline int gz_task_stop(int pid) { return GZ_SC1(SYS_TASK_STOP, pid); }
static inline int gz_proc_set_bg(int pid, int state) { return GZ_SC2(SYS_PROC_BG, pid, state); }
/* GZ_NOTIFY_CHILD on `port` (the caller's) whenever a child process exits. */
static inline int gz_child_notify(int port) { return GZ_SC1(SYS_CHILD_NOTIFY, port); }

static inline const void *gz_romfs_map(const char *name, uint32_t *size)
{
    int r = GZ_SC2(SYS_ROMFS_MAP, name, size);
    return r < 0 ? NULL : (const void *)r;
}

#endif
