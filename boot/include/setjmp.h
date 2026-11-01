/* setjmp/longjmp for the SPL (boot/setjmp.S), which puff (third_party/puff)
 * uses to stop on truncated input. The callee-saved registers of the o32
 * ABI, soft-float: s0-s7, fp, sp, ra, gp. */
#ifndef GLAZE_SPL_SETJMP_H
#define GLAZE_SPL_SETJMP_H

typedef unsigned long jmp_buf[12];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif
