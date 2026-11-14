/* Bluetooth must keep the CPU running; removal restores normal STANDBY. */
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define GLAZE_MIPS_H
#define GLAZE_HW_H
#define ST_IE 1u
#define ST_EXL 2u
#define ST_KSU_USER 16u
#define ST_IM2 0x400u
#define ST_IM7 0x8000u
static jmp_buf done;
static uint32_t status;
static unsigned slept, accounted;
static bool bluetooth;
static uint32_t irq_save(void);
static void irq_restore(uint32_t);
static void vr41xx_standby(void);
#include "../../kernel/sched.c"
static uint32_t irq_save(void)
{
    uint32_t before = status;
    status &= ~ST_IE;
    return before;
}
static void irq_restore(uint32_t value)
{
    status = value;
    if (bluetooth) {
        assert(!slept && !accounted && !standby_since);
        longjmp(done, 1);
    }
}
static void vr41xx_standby(void)
{
    assert(!bluetooth && (status & ST_IE) && standby_since == 100 && !need_resched);
    slept++;
    /* Hardware enables IE and a pending device IRQ wakes the idle thread. */
    status |= ST_IE;
    sched_standby_wake();
    longjmp(done, 1);
}
bool bt_present(void) { return bluetooth; }
uint64_t time_us(void) { return slept ? 125 : 100; }
void battery_idle(uint32_t us) { assert(us == 25); accounted++; }
void *page_alloc_n(uint32_t n) { (void)n; abort(); }
void page_free(void *p) { (void)p; abort(); }
void as_activate(struct addrspace *as) { (void)as; abort(); }
void swtch(struct context *old, struct context *next) { (void)old; (void)next; abort(); }
void ipc_timeout(struct thread *t) { (void)t; abort(); }
void sem_timeout(struct thread *t) { (void)t; abort(); }
void ipc_fail_reply_waiters(struct proc *p, struct thread *t) { (void)p; (void)t; abort(); }
void proc_reap(void) { abort(); }
void panic(const char *fmt, ...) { (void)fmt; abort(); }
void trap_return(void) { abort(); }
int main(void)
{
    status = ST_IM2 | ST_IM7 | ST_IE;
    bluetooth = true;
    if (!setjmp(done)) idle_loop();
    assert(status == (ST_IM2 | ST_IM7 | ST_IE) && !slept && !accounted);
    bluetooth = false;
    if (!setjmp(done)) idle_loop();
    assert(slept == 1 && accounted == 1 && !standby_since);
    assert(status == (ST_IM2 | ST_IM7 | ST_IE));
    puts("scheduler idle: Bluetooth inhibits STANDBY; removal restores sleep, IRQ wake and accounting PASS");
    return 0;
}
