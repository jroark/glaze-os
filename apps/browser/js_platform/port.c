/* Jerry's context and fatal allocation boundary are private to the browser. */
#include "../../../third_party/jerryscript/jerryscript.h"
#include "../plat.h"
#include "port.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static struct jerry_context_t *context;
jmp_buf js_failure_boundary;
bool js_failure_guard;
void jerry_port_init(void) {}
size_t jerry_port_context_alloc(size_t size)
{
    size_t total = size + 1024u * 1024u + 8;
    context = malloc(total);
    if (!context)
        jerry_port_fatal(JERRY_FATAL_OUT_OF_MEMORY);
    return total;
}
struct jerry_context_t *jerry_port_context_get(void) { return context; }
void js_port_discard(void)
{
    free(context);
    context = NULL;
}
void jerry_port_context_free(void) { js_port_discard(); }
void jerry_port_fatal(jerry_fatal_code_t code)
{
    printf("browser: JavaScript engine stopped (%u)\n", (unsigned)code);
    if (js_failure_guard)
        longjmp(js_failure_boundary, 1);
    exit(1);
}
void jerry_port_log(const char *message) { printf("browser JS: %s", message); }
double jerry_port_current_time(void) { return (double)plat_time() * 1000.0; }
int32_t jerry_port_local_tza(double unix_ms)
{
    (void)unix_ms;
    return 0;
}
unsigned glaze_js_random(void)
{
    static uint32_t state;
    if (!state)
        state = plat_ms() ^ 0x8bc3a751;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state & 0x7fffffff;
}
/* The browser supplies an asynchronous policy-aware module resolver. Never
 * let Jerry's optional default resolver bypass it with filesystem access. */
jerry_char_t *jerry_port_path_normalize(const jerry_char_t *path,jerry_size_t size)
{ (void)path;(void)size;return NULL; }
void jerry_port_path_free(jerry_char_t *path) { free(path); }
jerry_size_t jerry_port_path_base(const jerry_char_t *path) { (void)path;return 0; }
jerry_char_t *jerry_port_source_read(const char *path,jerry_size_t *size)
{ (void)path;*size=0;return NULL; }
void jerry_port_source_free(jerry_char_t *buffer) { free(buffer); }
