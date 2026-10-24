#ifndef GLAZE_JS_SETJMP_H
#define GLAZE_JS_SETJMP_H
#ifdef __mips__
#include <stdint.h>
typedef uint32_t jmp_buf[12];
int glaze_js_setjmp(jmp_buf env) __attribute__((returns_twice));
void glaze_js_longjmp(jmp_buf env,int value) __attribute__((noreturn));
#define setjmp glaze_js_setjmp
#define longjmp glaze_js_longjmp
#else
#include_next <setjmp.h>
#endif
#endif
