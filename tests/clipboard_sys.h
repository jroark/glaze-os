#ifndef CLIPBOARD_TEST_SYS_H
#define CLIPBOARD_TEST_SYS_H
#define GLAZE_SYS_H
#include <glaze/abi.h>
int gz_send(int port, const struct gz_msg *m, struct gz_msg *r);
int gz_getpid(void);
#endif
