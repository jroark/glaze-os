/* errno, for the POSIX-style interfaces (sockets). Values follow Linux, as
 * lwIP's do. */
#ifndef GLAZE_ERRNO_H
#define GLAZE_ERRNO_H

/* Per thread: it lives in the thread's block (glaze/thread.h). */
int *__gz_errno(void);
#define errno (*__gz_errno())

#define EPERM           1
#define ENOENT          2
#define EINTR           4
#define EIO             5
#define ENXIO           6
#define EBADF           9
#define EAGAIN          11
#define EWOULDBLOCK     EAGAIN
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define EBUSY           16
#define ENODEV          19
#define EINVAL          22
#define ENFILE          23
#define EMFILE          24
#define ENOSPC          28
#define EPIPE           32
#define ERANGE          34
#define ENOSYS          38
#define ENOTSOCK        88
#define EDESTADDRREQ    89
#define EMSGSIZE        90
#define EPROTOTYPE      91
#define ENOPROTOOPT     92
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP      95
#define EAFNOSUPPORT    97
#define EADDRINUSE      98
#define EADDRNOTAVAIL   99
#define ENETDOWN        100
#define ENETUNREACH     101
#define ENETRESET       102
#define ECONNABORTED    103
#define ECONNRESET      104
#define ENOBUFS         105
#define EISCONN         106
#define ENOTCONN        107
#define ESHUTDOWN       108
#define ETIMEDOUT       110
#define ECONNREFUSED    111
#define EHOSTDOWN       112
#define EHOSTUNREACH    113
#define EALREADY        114
#define EINPROGRESS     115

#endif
