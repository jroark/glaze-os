#include <glaze/sys.h>
#include <stdlib.h>
void __glaze_ld_finish(void);
void exit(int code)
{
    __glaze_ld_finish();
    gz_exit(code);
    for (;;) ;
}

/* No signals: an abnormal exit, with the status a shell gives SIGABRT. */
void abort(void)
{
    gz_exit(134);
    __builtin_unreachable();
}
