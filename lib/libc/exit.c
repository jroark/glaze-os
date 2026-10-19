#include <glaze/sys.h>
#include <stdlib.h>
void __glaze_ld_finish(void);
void exit(int code)
{
    __glaze_ld_finish();
    gz_exit(code);
    for (;;) ;
}
