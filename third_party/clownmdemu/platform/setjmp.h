#ifndef GLAZE_MD_SETJMP_H
#define GLAZE_MD_SETJMP_H
#ifdef __mips__
#include <stdint.h>
typedef uint32_t jmp_buf[12];
int glaze_md_setjmp(jmp_buf env) __attribute__((returns_twice));
void glaze_md_longjmp(jmp_buf env,int value) __attribute__((noreturn));
#define setjmp glaze_md_setjmp
#define longjmp glaze_md_longjmp
#else
#include_next <setjmp.h>
#endif
#endif
