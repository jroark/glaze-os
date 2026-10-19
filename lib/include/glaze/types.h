/* Base types shared by the kernel and userspace (compiler freestanding headers). */
#ifndef GLAZE_GLAZE_TYPES_H
#define GLAZE_GLAZE_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a)  (((x) + (a) - 1) & ~((a) - 1))
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))

#endif
