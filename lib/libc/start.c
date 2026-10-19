#include <glaze/sys.h>
#include <stdlib.h>

int main(void);

void __libc_start(int pid)
{
    (void)pid;
    exit(main());
}

void exit(int code)
{
    gz_exit(code);
    for (;;)
        ;
}
