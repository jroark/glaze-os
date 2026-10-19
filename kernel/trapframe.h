/* Trap frame layout shared by vectors.S and C. */
#ifndef GLAZE_TRAPFRAME_H
#define GLAZE_TRAPFRAME_H

#define TF_REG(n)       ((n) * 4)
#define TF_HI           (32 * 4)
#define TF_LO           (33 * 4)
#define TF_EPC          (34 * 4)
#define TF_STATUS       (35 * 4)
#define TF_CAUSE        (36 * 4)
#define TF_BADVADDR     (37 * 4)
#define TF_SIZE         (40 * 4)

#ifndef __ASSEMBLER__
#include "types.h"

struct trapframe {
    uint32_t regs[32];
    uint32_t hi, lo;
    uint32_t epc, status, cause, badvaddr;
    uint32_t pad[2];
};

#define R_V0    2
#define R_V1    3
#define R_A0    4
#define R_A1    5
#define R_A2    6
#define R_A3    7
#define R_SP    29
#define R_RA    31

/* Callee-saved register context for swtch(). */
struct context {
    uint32_t s[9];
    uint32_t sp, ra;
};

void swtch(struct context *old, struct context *new);
void trap_return(void);
void tlb_flush_all(void);
#endif

#endif
