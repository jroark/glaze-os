#ifndef GLAZE_STDLIB_H
#define GLAZE_STDLIB_H

#include <stddef.h>

void *malloc(size_t n);
void *calloc(size_t n, size_t m);
void *realloc(void *p, size_t n);
void free(void *p);
void exit(int code) __attribute__((noreturn));
int abs(int v);
int atoi(const char *s);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));

#endif
