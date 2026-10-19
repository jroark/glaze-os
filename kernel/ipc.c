/*
 * Synchronous message-passing IPC on ports (L4/QNX-style call/recv/reply),
 * plus asynchronous notification bits per port.
 *
 *   send(port, msg, reply)  - blocks until the owner replies
 *   recv(port, msg, tmo)    - returns a sender token, or GZ_RECV_NOTIFY
 *   reply(token, msg)       - unblocks the sender
 *   notify(port, bits)      - ORs bits into the port and wakes a receiver
 */

#include "kernel.h"
#include "proc.h"

struct port {
    bool used;
    struct proc *owner;
    char name[16];
    struct thread *sendq_head, *sendq_tail;
    struct thread *receiver;
    uint32_t notify_bits;
};

static struct port ports[MAX_PORTS];

void ipc_init(void)
{
    memset(ports, 0, sizeof(ports));
}

static struct port *port_get(int id)
{
    if (id < 1 || id > MAX_PORTS || !ports[id - 1].used)
        return NULL;
    return &ports[id - 1];
}

int ipc_port_create(struct proc *owner)
{
    for (int i = 0; i < MAX_PORTS; i++) {
        if (!ports[i].used) {
            memset(&ports[i], 0, sizeof(ports[i]));
            ports[i].used = true;
            ports[i].owner = owner;
            return i + 1;
        }
    }
    return GZ_EFULL;
}

int ipc_port_register(int id, const char *name)
{
    struct port *p = port_get(id);

    if (!p || p->owner != cur->proc)
        return GZ_EPERM;
    if (ipc_port_lookup(name) > 0)
        return GZ_EINVAL;
    strncpy(p->name, name, sizeof(p->name) - 1);
    return 0;
}

int ipc_port_lookup(const char *name)
{
    for (int i = 0; i < MAX_PORTS; i++) {
        if (ports[i].used && ports[i].name[0] && !strcmp(ports[i].name, name))
            return i + 1;
    }
    return GZ_ENOENT;
}

/* Deliver sender `s`'s message into the blocked receiver `r`. */
static void deliver(struct thread *r, struct thread *s)
{
    int rv = s->tid;

    if (copy_to_as(&r->proc->as, r->ipc_ubuf, &s->msg, sizeof(s->msg)))
        rv = GZ_EINVAL;
    s->state = T_REPLY;
    s->reply_owner_pid = r->proc->pid;
    r->tf->regs[R_V0] = (uint32_t)rv;
    thread_make_runnable(r);
}

static bool deliver_notify(struct thread *r, struct port *p)
{
    struct gz_msg m;

    memset(&m, 0, sizeof(m));
    m.w[0] = p->notify_bits;
    p->notify_bits = 0;
    if (copy_to_as(&r->proc->as, r->ipc_ubuf, &m, sizeof(m)))
        return false;
    r->tf->regs[R_V0] = GZ_RECV_NOTIFY;
    return true;
}

int ipc_send(int id, uint32_t umsg, uint32_t ureply)
{
    struct port *p = port_get(id);

    if (!p)
        return GZ_ENOENT;
    if (copy_from_as(&cur->proc->as, &cur->msg, umsg, sizeof(cur->msg)))
        return GZ_EINVAL;
    cur->ipc_ubuf = ureply;
    cur->ipc_port = id;

    if (p->receiver) {
        struct thread *r = p->receiver;
        p->receiver = NULL;
        deliver(r, cur);
        thread_block(T_REPLY);
    } else {
        cur->next = NULL;
        if (p->sendq_tail)
            p->sendq_tail->next = cur;
        else
            p->sendq_head = cur;
        p->sendq_tail = cur;
        thread_block(T_SEND);
    }
    return SYSRET_BLOCKED;
}

int ipc_recv(int id, uint32_t umsg, int timeout_ms)
{
    struct port *p = port_get(id);

    if (!p || p->owner != cur->proc)
        return GZ_EPERM;
    if (p->receiver)
        return GZ_EINVAL;
    cur->ipc_ubuf = umsg;
    cur->ipc_port = id;

    if (p->notify_bits)
        return deliver_notify(cur, p) ? GZ_RECV_NOTIFY : GZ_EINVAL;

    if (p->sendq_head) {
        struct thread *s = p->sendq_head;
        p->sendq_head = s->next;
        if (!p->sendq_head)
            p->sendq_tail = NULL;
        s->next = NULL;
        if (copy_to_as(&cur->proc->as, umsg, &s->msg, sizeof(s->msg)))
            return GZ_EINVAL;
        s->state = T_REPLY;
        s->reply_owner_pid = cur->proc->pid;
        return s->tid;
    }

    if (timeout_ms == 0)
        return GZ_ETIMEDOUT;
    p->receiver = cur;
    if (timeout_ms > 0)
        thread_block_timeout(T_RECV, time_us() + (uint64_t)timeout_ms * 1000u);
    else
        thread_block(T_RECV);
    return SYSRET_BLOCKED;
}

void ipc_timeout(struct thread *t)
{
    struct port *p = port_get(t->ipc_port);

    if (p && p->receiver == t)
        p->receiver = NULL;
    t->tf->regs[R_V0] = (uint32_t)GZ_ETIMEDOUT;
    thread_make_runnable(t);
}

int ipc_reply(int token, uint32_t umsg)
{
    struct thread *s = thread_by_tid(token);
    struct gz_msg m;

    if (!s || s->state != T_REPLY || s->reply_owner_pid != cur->proc->pid)
        return GZ_ENOENT;
    if (copy_from_as(&cur->proc->as, &m, umsg, sizeof(m)))
        return GZ_EINVAL;
    if (s->ipc_ubuf && copy_to_as(&s->proc->as, s->ipc_ubuf, &m, sizeof(m)))
        s->tf->regs[R_V0] = (uint32_t)GZ_EINVAL;
    else
        s->tf->regs[R_V0] = 0;
    thread_make_runnable(s);
    return 0;
}

int ipc_notify(int id, uint32_t bits)
{
    struct port *p = port_get(id);

    if (!p)
        return GZ_ENOENT;
    p->notify_bits |= bits;
    if (p->receiver) {
        struct thread *r = p->receiver;
        p->receiver = NULL;
        if (!deliver_notify(r, p))
            r->tf->regs[R_V0] = (uint32_t)GZ_EINVAL;
        thread_make_runnable(r);
    }
    return 0;
}

/* Tear down IPC state for an exiting process. */
void ipc_proc_cleanup(struct proc *dead)
{
    for (int i = 0; i < MAX_PORTS; i++) {
        struct port *p = &ports[i];
        if (!p->used)
            continue;

        /* Remove the dead process's threads from other ports' queues. */
        struct thread **link = &p->sendq_head;
        p->sendq_tail = NULL;
        while (*link) {
            struct thread *t = *link;
            if (t->proc == dead) {
                *link = t->next;
                t->next = NULL;
            } else {
                p->sendq_tail = t;
                link = &t->next;
            }
        }
        if (p->receiver && p->receiver->proc == dead)
            p->receiver = NULL;

        /* Destroy the dead process's ports, failing queued senders. */
        if (p->owner == dead) {
            struct thread *t = p->sendq_head;
            while (t) {
                struct thread *n = t->next;
                t->next = NULL;
                t->tf->regs[R_V0] = (uint32_t)GZ_EDEAD;
                thread_make_runnable(t);
                t = n;
            }
            memset(p, 0, sizeof(*p));
        }
    }
}

/* Called by proc_exit for every live thread awaiting a reply from `dead`. */
void ipc_fail_reply_waiters(struct proc *dead, struct thread *t)
{
    if (t->state == T_REPLY && t->reply_owner_pid == dead->pid) {
        t->tf->regs[R_V0] = (uint32_t)GZ_EDEAD;
        thread_make_runnable(t);
    }
}
