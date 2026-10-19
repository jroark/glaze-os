/* POSIX descriptors: only sockets exist (see sys/socket.h). */
#ifndef GLAZE_UNISTD_H
#define GLAZE_UNISTD_H

#include <sys/types.h>

int close(int fd);
ssize_t read(int fd, void *buf, size_t len);
ssize_t write(int fd, const void *buf, size_t len);

#endif
