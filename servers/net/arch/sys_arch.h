/*
 * lwIP OS port for Glaze: threads, semaphores, mutexes and mailboxes on
 * the kernel's process-private semaphores (glaze/sys.h). There's no LL/SC
 * on the VR4131, so even mutexes are kernel objects.
 */
#ifndef GLAZE_LWIP_SYS_ARCH_H
#define GLAZE_LWIP_SYS_ARCH_H

#include <glaze/types.h>

typedef struct {
    int id;                 /* kernel semaphore, 0 = invalid */
} sys_sem_t;

typedef struct {
    int id;
} sys_mutex_t;

struct sys_mbox;
typedef struct {
    struct sys_mbox *mb;
} sys_mbox_t;

typedef int sys_thread_t;
typedef int sys_prot_t;

#define SYS_MBOX_NULL   { NULL }
#define SYS_SEM_NULL    { 0 }

/* lwIP's per-thread semaphore (LWIP_NETCONN_SEM_PER_THREAD), kept in the
 * thread's block (glaze/thread.h). */
sys_sem_t *sys_arch_netconn_sem_get(void);
void sys_arch_netconn_sem_free(void);
#define LWIP_NETCONN_THREAD_SEM_GET()   sys_arch_netconn_sem_get()
#define LWIP_NETCONN_THREAD_SEM_ALLOC() ((void)sys_arch_netconn_sem_get())
#define LWIP_NETCONN_THREAD_SEM_FREE()  sys_arch_netconn_sem_free()

#define sys_sem_valid(s)            ((s)->id > 0)
#define sys_sem_set_invalid(s)      ((s)->id = 0)
#define sys_mutex_valid(m)          ((m)->id > 0)
#define sys_mutex_set_invalid(m)    ((m)->id = 0)
#define sys_mbox_valid(m)           ((m)->mb != NULL)
#define sys_mbox_set_invalid(m)     ((m)->mb = NULL)

#endif
