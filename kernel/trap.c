/* Exception dispatch. */

#include "kernel.h"
#include "hw.h"
#include "mips.h"
#include "proc.h"

extern char vec_refill[], vec_refill_end[];
extern char vec_general[], vec_general_end[];

#include "drivers.h"

static const char *const exc_names[32] = {
    [EXC_INT] = "Int", [EXC_MOD] = "TLB Mod", [EXC_TLBL] = "TLB load",
    [EXC_TLBS] = "TLB store", [EXC_ADEL] = "Address load",
    [EXC_ADES] = "Address store", [EXC_SYS] = "Syscall", [EXC_BP] = "Break",
    [EXC_RI] = "Reserved instruction", [EXC_CPU] = "Coprocessor unusable",
    [EXC_OV] = "Overflow",
};

void trap_init(void)
{
    memcpy((void *)0x80000000, vec_refill, vec_refill_end - vec_refill);
    memcpy((void *)0x80000180, vec_general, vec_general_end - vec_general);

    /* 4 KiB pages everywhere; nothing wired; start from a clean TLB. */
    mtc0($5, PAGEMASK_4K);
    mtc0($6, 0);
    tlb_flush_all();

    /* Leave BEV: vectors now live in KSEG0. Interrupts stay off until the
     * first eret into a thread. */
    write_c0_status(read_c0_status() & ~(ST_BEV | ST_IE | ST_ERL));
}

static void dump_frame(struct trapframe *tf)
{
    kprintf("  epc=0x%08x cause=0x%08x status=0x%08x badvaddr=0x%08x\n",
            tf->epc, tf->cause, tf->status, tf->badvaddr);
    kprintf("  ra=0x%08x sp=0x%08x a0=0x%08x v0=0x%08x\n",
            tf->regs[R_RA], tf->regs[R_SP], tf->regs[R_A0], tf->regs[R_V0]);
}

void trap(struct trapframe *tf)
{
    uint32_t exc = CAUSE_EXC(tf->cause);
    bool from_user = (tf->status & ST_KSU_USER) != 0;

    if (exc == EXC_INT) {
        uint32_t pending = tf->cause & tf->status & 0xFF00;
        if (pending & CAUSE_IP7)
            timer_irq();
        if (pending & CAUSE_IP2)
            irq_dispatch_ip2();
    } else if (exc == EXC_SYS && from_user) {
        syscall_dispatch(tf);
    } else if (from_user && (exc == EXC_TLBL || exc == EXC_TLBS) &&
               !as_fault(&cur->proc->as, tf->badvaddr, exc == EXC_TLBS)) {
        /* A demand-paged page (romfs) came in; retry the access. */
    } else if (from_user) {
        kprintf("fault: %s in %s (pid %d)\n",
                exc_names[exc] ? exc_names[exc] : "?", cur->proc->name, cur->proc->pid);
        dump_frame(tf);
        proc_exit(cur->proc, -1);
    } else {
        kprintf("kernel exception: %s\n", exc_names[exc] ? exc_names[exc] : "?");
        dump_frame(tf);
        panic("unhandled kernel exception");
    }

    if (need_resched || cur->state != T_RUNNING)
        schedule();
}
