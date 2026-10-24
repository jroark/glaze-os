/* JerryScript browser profile; identical engine features on host and MIPS. */
#define JERRY_NDEBUG 1
#define JERRY_EXTERNAL_CONTEXT 1
#define JERRY_GLOBAL_HEAP_SIZE 1024
#define JERRY_CPOINTER_32_BIT 1
#define JERRY_VM_HALT 1
#ifdef __mips__
#define JERRY_STACK_LIMIT 24
#else
/* ASan and 64-bit native frames need more space for the same JS nesting. */
#define JERRY_STACK_LIMIT 128
#endif
#define JERRY_ERROR_MESSAGES 1
#define JERRY_FUNCTION_TO_STRING 1
#define JERRY_LINE_INFO 1
#define JERRY_MODULE_SYSTEM 0
#define JERRY_BUILTIN_BIGINT 0
