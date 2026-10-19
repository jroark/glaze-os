#ifndef GLAZE_FCNTL_H
#define GLAZE_FCNTL_H

#define F_GETFL     3
#define F_SETFL     4
#define O_RDWR      0x2
#define O_NONBLOCK  0x800

int fcntl(int fd, int cmd, ...);

#endif
