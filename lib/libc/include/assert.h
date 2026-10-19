/* assert() logs the failed expression and exits the process. */
#ifndef GLAZE_ASSERT_H
#define GLAZE_ASSERT_H

void __assert_fail(const char *expr, const char *file, int line) __attribute__((noreturn));

#ifdef NDEBUG
#define assert(x)   ((void)0)
#else
#define assert(x)   ((x) ? (void)0 : __assert_fail(#x, __FILE__, __LINE__))
#endif

#endif
