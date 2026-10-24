#ifndef GLAZE_JS_PORT_H
#define GLAZE_JS_PORT_H
#include <setjmp.h>
#include <stdbool.h>
extern jmp_buf js_failure_boundary;
extern bool js_failure_guard;
void js_port_discard(void);
#endif
