/* Processes, threads, address spaces, ports. */
#ifndef GLAZE_PROC_H
#define GLAZE_PROC_H

#include "types.h"
#include "trapframe.h"
#include <glaze/abi.h>

#define PAGE_SIZE       4096u
#define PAGE_SHIFT      12
#define KSTACK_SIZE     8192u

#define MAX_PROCS       32
#define MAX_THREADS     64
#define MAX_PORTS       64
#define MAX_REGIONS     32
#define MAX_SHM         32

#define USER_TEXT_BASE  0x00400000u
#define USER_MMAP_BASE  0x10000000u
#define USER_STACK_TOP  0x7FFF0000u
#define USER_STACK_SIZE (64u * 1024u)
#define USER_TOP        0x80000000u

/* ---- memory (mm.c) ------------------------------------------------------ */

typedef uint32_t pte_t;

void mm_init(void);
void *page_alloc(void);             /* zeroed KSEG0 page, or NULL */
void *page_alloc_n(uint32_t n);      /* contiguous */
void page_free(void *kva);
uint32_t mm_free_pages(void);
uint32_t mm_total_pages(void);

enum region_kind { REGION_OWNED = 1, REGION_SHM, REGION_DEVICE };

struct region {
    uint32_t va, npages;
    int kind;
    int shm_id;
};

struct addrspace {
    pte_t **pgd;                    /* 1024 entries, KSEG0 page-table ptrs */
    uint8_t asid;
    uint32_t mmap_top;
    struct region regions[MAX_REGIONS];
};

int as_create(struct addrspace *as, uint8_t asid);
void as_destroy(struct addrspace *as);
int as_map_page(struct addrspace *as, uint32_t va, uint32_t pa, uint32_t flags);
pte_t as_lookup(struct addrspace *as, uint32_t va);
int as_alloc_region(struct addrspace *as, uint32_t va, uint32_t len, int kind);
uint32_t as_mmap_anon(struct addrspace *as, uint32_t len);
uint32_t as_map_phys(struct addrspace *as, uint32_t pa, uint32_t len, uint32_t cache);
void as_activate(struct addrspace *as);
uint32_t as_map_frames(struct addrspace *as, const uint32_t *frames, uint32_t npages, int shm_id);

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
};

#define PROC_PRIVILEGED     0x1     /* may map the framebuffer, bind input */

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
struct proc *proc_by_pid(int pid);
void proc_exit(struct proc *p, int code);
void proc_reap(void);
void sched_kill_proc_threads(struct proc *p);
int sched_reap_threads(struct proc *p);
int proc_count(void);

/* ---- IPC (ipc.c) -------------------------------------------------------- */

void ipc_init(void);
int ipc_port_create(struct proc *owner);
int ipc_port_register(int port, const char *name);
int ipc_port_lookup(const char *name);
int ipc_send(int port, uint32_t umsg, uint32_t ureply);
int ipc_recv(int port, uint32_t umsg, int timeout_ms);
int ipc_reply(int token, uint32_t umsg);
int ipc_notify(int port, uint32_t bits);
void ipc_proc_cleanup(struct proc *p);
void ipc_timeout(struct thread *t);

/* ---- misc --------------------------------------------------------------- */

/* timer.c */
void timer_init(void);
void timer_irq(void);
uint64_t time_us(void);

/* romfs.c */
const void *romfs_find(const char *name, uint32_t *size);
void romfs_list(void);

/* elf.c */
int elf_load(struct addrspace *as, const void *image, uint32_t size, uint32_t *entry);

/* syscall.c */
void syscall_dispatch(struct trapframe *tf);
#define SYSRET_BLOCKED  0x7FFFFFFF

/* trap.c */
void trap_init(void);
void trap(struct trapframe *tf);

#endif
