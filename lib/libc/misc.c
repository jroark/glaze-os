#include <stdlib.h>
#include <string.h>

int abs(int v)
{
    return v < 0 ? -v : v;
}

int atoi(const char *s)
{
    int sign = 1, v = 0;

    while (*s == ' ')
        s++;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return sign * v;
}

/* Insertion sort: lists in Glaze are tiny (notes, settings rows). */
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    char *a = base;
    char tmp[256];

    if (size > sizeof(tmp))
        return;
    for (size_t i = 1; i < n; i++) {
        memcpy(tmp, a + i * size, size);
        size_t j = i;
        while (j > 0 && cmp(a + (j - 1) * size, tmp) > 0) {
            memcpy(a + j * size, a + (j - 1) * size, size);
            j--;
        }
        memcpy(a + j * size, tmp, size);
    }
}
