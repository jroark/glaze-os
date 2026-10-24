/* Processes, threads, address spaces, ports. */
#ifndef GLAZE_PROC_H
#define GLAZE_PROC_H

#include "types.h"
#include "trapframe.h"
#include <glaze/abi.h>
#include <glaze/loader.h>

#define PAGE_SIZE       4096u
#define PAGE_SHIFT      12
#define KSTACK_SIZE     8192u

#define MAX_PROCS       32
#define MAX_THREADS     128
#define MAX_SEMS        256
#define MAX_STACK_SLOTS 32          /* threads besides main, per process */
#define MAX_PORTS       64
#define MAX_SHM         48

#define USER_TEXT_BASE  0x00400000u
#define USER_MMAP_BASE  0x10000000u
#define USER_STACK_TOP  0x7FFF0000u
#define USER_STACK_SIZE (64u * 1024u)
#define USER_TOP        0x80000000u

/* ---- memory (mm.c) ------------------------------------------------------ */

typedef uint32_t pte_t;

/* main.c: a RAM boot's parameters (glaze/bootinfo.h), or NULL from NAND. */
struct gz_bootinfo;
extern const struct gz_bootinfo *ram_boot;

void mm_init(void);                 /* reserves a RAM boot's romfs and RAM disk */
void *page_alloc_colour(uint32_t va);
void mm_sync_code(void *kva, uint32_t len);
void *page_alloc(void);             /* zeroed KSEG0 page, or NULL */
void *page_alloc_n(uint32_t n);      /* contiguous */
void page_free(void *kva);
uint32_t mm_free_pages(void);
uint32_t mm_total_pages(void);

/* REGION_ROMFS: a romfs file (shm_id is its entry index), faulted in. */
enum region_kind { REGION_OWNED = 1, REGION_SHM, REGION_DEVICE, REGION_IMAGE, REGION_ROMFS, REGION_LOADER };

struct region {
    uint32_t va, npages;
    int kind;
    int shm_id;
};

struct addrspace {
    pte_t **pgd;                    /* 1024 entries, KSEG0 page-table ptrs */
    uint8_t asid;
    uint32_t mmap_top;
    struct region_page *regions;
};

struct region_page {
    struct region_page *next;
    struct region entry[(PAGE_SIZE - sizeof(void *)) / sizeof(struct region)];
};
struct region *as_region_add(struct addrspace *, uint32_t, uint32_t, int, int);
void as_region_drop(struct addrspace *, struct region *);
void as_unmap_page(struct addrspace *, uint32_t, bool);
void as_prune_tables(struct addrspace *);
struct romfs_entry;
int image_map(struct addrspace *, const struct romfs_entry *, uint32_t, struct gz_image_info *, uint32_t);
int image_map_exec(struct addrspace *, struct addrspace *, const struct romfs_entry *, uint32_t, uint32_t,
                   struct gz_image_info *, uint32_t *, char *, uint32_t);
int image_map_user(struct addrspace *, uint32_t, uint32_t, uint32_t, uint32_t);
int image_release(struct addrspace *, uint32_t);
void image_release_region(struct addrspace *, struct region *);
int image_seal(struct addrspace *, uint32_t);
int image_query(struct addrspace *, uint32_t, uint32_t);
int image_stats(struct addrspace *, uint32_t);
int image_fault(struct addrspace *, struct region *, uint32_t va);
int romfs_fault(struct addrspace *, struct region *, uint32_t va);
/* A user access to an unmapped page of a demand-paged region; 0 if mapped. */
int as_fault(struct addrspace *as, uint32_t va, bool write);
uint32_t as_map_romfs(struct addrspace *as, uint32_t index, uint32_t size, uint32_t colour);

int as_create(struct addrspace *as, uint8_t asid);
void as_destroy(struct addrspace *as);
int as_map_page(struct addrspace *as, uint32_t va, uint32_t pa, uint32_t flags);
pte_t as_lookup(struct addrspace *as, uint32_t va);
int as_alloc_region(struct addrspace *as, uint32_t va, uint32_t len, int kind);
uint32_t as_mmap_anon(struct addrspace *as, uint32_t len);
int as_loader_map(struct addrspace *, uint32_t, uint32_t);
int as_loader_publish(struct addrspace *, uint32_t, uint32_t, uint32_t);
int as_loader_release(struct addrspace *, uint32_t);
uint32_t as_map_phys(struct addrspace *as, uint32_t pa, uint32_t len, uint32_t cache);
void as_activate(struct addrspace *as);
uint32_t as_map_frames(struct addrspace *as, const uint32_t *frames, uint32_t npages, int shm_id);
int as_unmap_shm(struct addrspace *as, uint32_t va);

int copy_to_as(struct addrspace *as, uint32_t uva, const void *src, uint32_t len);
int copy_from_as(struct addrspace *as, void *dst, uint32_t uva, uint32_t len);
int copy_str_from_as(struct addrspace *as, char *dst, uint32_t uva, uint32_t max);

extern pte_t **cur_pgd;

/* ---- shared memory (shm.c) ---------------------------------------------- */

int shm_create(uint32_t len);
uint32_t shm_map(struct addrspace *as, int id);
void shm_unref(int id);
void shm_ref(int id);

/* ---- threads and processes (sched.c, proc.c) ---------------------------- */

enum tstate {
    T_UNUSED = 0,
    T_RUNNABLE,
    T_RUNNING,
    T_SLEEP,
    T_SEND,         /* queued on a port, waiting for a receiver */
    T_REPLY,        /* message delivered, waiting for reply */
    T_RECV,         /* waiting for a message or notification */
    T_SEM,          /* waiting on a semaphore */
    T_ZOMBIE,
};

enum { PRIO_HIGH = 0, PRIO_NORMAL = 1, PRIO_LOW = 2, PRIO_IDLE = 3, NPRIO = 4 };

struct proc;

struct thread {
    int tid;
    enum tstate state;
    int prio;
    struct proc *proc;
    struct context ctx;
    uint8_t *kstack;
    struct trapframe *tf;
    struct thread *next;            /* run queue or port send queue */
    uint64_t wake_us;               /* 0 = no timeout */
    bool timed;                     /* on the timeout list */

    /* IPC state. */
    struct gz_msg msg;              /* message being sent */
    uint32_t ipc_ubuf;              /* user buffer for recv / reply */
    int ipc_port;                   /* port blocked on (send/recv) */
    int reply_owner_pid;            /* pid that owes us a reply */

    int sem_id;                     /* semaphore waited on (T_SEM) */
    int stack_slot;                 /* user stack slot, -1 for the main thread */
};

struct proc {
    int pid;
    bool used;
    char name[32];
    struct addrspace as;
    struct thread *main;
    uint32_t flags;
    int parent;
    bool dying;
    int exit_code;
    uint32_t stack_slots;           /* bitmap of thread stacks in use */
    uint64_t cpu_us;                /* accumulated scheduled time, all threads */
    bool frozen;                    /* GZ_PROC_SUSPENDED: runnable threads stay off the run queue */
    int child_port;                 /* own port told GZ_NOTIFY_CHILD on a child's exit, or 0 */
};

#define PROC_PRIVILEGED     0x1     /* may map the framebuffer, bind input */
#define PROC_FLASH          0x2     /* raw NAND (kernel-granted, file server) */
#define PROC_NET            0x4     /* Ethernet frames (kernel-granted, net server) */
#define PROC_AUDIO          0x8     /* wave output (kernel-granted, audio server) */
#define PROC_APP            0x10    /* managed foreground/background app, may be ended */
#define PROC_BACKGROUND     0x20    /* app is currently in the background (PRIO_LOW) */

extern struct thread *cur;
extern uint32_t cur_kstack_top;

void sched_init(void);
struct thread *thread_create_user(struct proc *p, uint32_t entry, uint32_t sp, uint32_t arg, int prio);
void thread_make_runnable(struct thread *t);
void thread_block(enum tstate st);
void thread_sleep_until(uint64_t wake_us);
void thread_cancel_timeout(struct thread *t);
void thread_block_timeout(enum tstate st, uint64_t wake_us);
void thread_free(struct thread *t);
void sched_timer_tick(uint64_t now_us);
void schedule(void);
void sched_start(void) __attribute__((noreturn));
struct thread *thread_by_tid(int tid);
extern bool need_resched;

struct proc *proc_spawn(const char *path, int parent, uint32_t flags);
struct proc *proc_spawn_image(struct addrspace *src_as, uint32_t src, uint32_t size, const char *name,
                              int parent, uint32_t flags);
struct proc *proc_by_pid(int pid);
void proc_exit(struct proc *p, int code);
void proc_reap(void);
void sched_kill_proc_threads(struct proc *p);
void sched_set_proc_prio(struct proc *p, int prio);
void sched_freeze_proc(struct proc *p, bool frozen);
int sched_reap_threads(struct proc *p);
int sched_live_threads(struct proc *p);
uint64_t sched_account(uint64_t now);
void sched_task_info(struct proc *p, struct gz_task_info *info);
int proc_count(void);
int proc_snapshot(struct addrspace *as, uint32_t out, uint32_t size);
int proc_task_stop(int pid);
int proc_set_bg(int pid, int state);
int proc_child_notify(int port);
int thread_spawn(uint32_t entry, uint32_t arg);
int thread_exit(void);

/* ---- semaphores (sem.c) ------------------------------------------------- */

int sem_create(struct proc *p, int count);
int sem_wait(int id, int timeout_ms);
int sem_post(int id);
int sem_destroy(int id);
void sem_timeout(struct thread *t);
void sem_proc_cleanup(struct proc *p);

/* ---- IPC (ipc.c) -------------------------------------------------------- */

void ipc_init(void);
int ipc_port_create(struct proc *owner);
int ipc_port_register(int port, const char *name);
int ipc_port_lookup(const char *name);
int ipc_send(int port, uint32_t umsg, uint32_t ureply);
int ipc_recv(int port, uint32_t umsg, int timeout_ms);
int ipc_reply(int token, uint32_t umsg);
int ipc_notify(int port, uint32_t bits);
bool ipc_port_owned(int port, const struct proc *p);
void ipc_proc_cleanup(struct proc *p);
void ipc_timeout(struct thread *t);

/* ---- misc --------------------------------------------------------------- */

/* timer.c */
void timer_init(void);
void timer_irq(void);
uint64_t time_us(void);

/* romfs.c: the archive in NAND, paged in through a shared cache. */
struct romfs_entry {
    char name[48];
    uint32_t offset;            /* from the archive start, page aligned */
    uint32_t size;
};
void romfs_init(void);
void romfs_list(void);
const struct romfs_entry *romfs_lookup(const char *name);
uint32_t romfs_index(const struct romfs_entry *e);
const struct romfs_entry *romfs_entry_at(uint32_t index);
uint32_t romfs_frame(const struct romfs_entry *e, uint32_t off);         /* loads; 0 if out of memory */
uint32_t romfs_cached_frame(const struct romfs_entry *e, uint32_t off);  /* no load; 0 if absent */
const uint8_t *romfs_ptr(const struct romfs_entry *e, uint32_t off);     /* until the next allocation */
int romfs_read(const struct romfs_entry *e, uint32_t off, void *dst, uint32_t len);
bool romfs_is_cache_frame(uint32_t pa);
void romfs_map_ref(uint32_t pa);
void romfs_map_unref(uint32_t pa);
uint32_t romfs_reclaim(uint32_t n);
uint32_t romfs_cache_pages(void);
uint32_t romfs_reclaimable_pages(void);

/* elf.c */
int elf_load(struct addrspace *as, struct addrspace *src_as, const struct romfs_entry *rom, uint32_t src, uint32_t size,
             uint32_t *entry, struct gz_ld_start *start);

/* syscall.c */
void syscall_dispatch(struct trapframe *tf);
#define SYSRET_BLOCKED  0x7FFFFFFF

/* trap.c */
void trap_init(void);
void trap(struct trapframe *tf);

#endif
