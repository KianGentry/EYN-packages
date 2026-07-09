#pragma once

// Minimal errno support.

extern int errno;

#define ENOENT  2
#define EINTR   4
#define EAGAIN  11
#define EINVAL  22
#define ENOMEM  12
#define ENOSYS  38
#define EBADF   9   /* bad file descriptor */
#define EACCES  13  /* permission denied */
#define EEXIST  17  /* file already exists */
#define EISDIR  21  /* is a directory */
#define ENOTDIR 20  /* not a directory */
#define ENOTEMPTY 39
#define EFAULT  14  /* bad address */
#define EIO     5   /* I/O error */
#define ENXIO   6
#define EPIPE   32
#define ERANGE  34
#define E2BIG   7
#define ESRCH   3
#define ELOOP   40
#define EOPNOTSUPP 95
#define EPERM   1
#define ENAMETOOLONG 36
#define EBUSY   16
#define EXDEV   18
#define ENOTBLK 15
#define EROFS   30
#define ENOTSUP 95
#define ENODATA 61
#define ENOMEDIUM 123
#define EBADR 53

/* Socket-related errno codes (values match Linux i386 for compatibility). */
#define ENOTSOCK 88
#define EMSGSIZE 90
#define EPROTOTYPE 91
#define ENOPROTOOPT 92
#define EPROTONOSUPPORT 93
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define EADDRNOTAVAIL 99
#define ENETDOWN 100
#define ENETUNREACH 101
#define ENETRESET 102
#define ECONNABORTED 103
#define ECONNRESET 104
#define ENOBUFS 105
#define EISCONN 106
#define ENOTCONN 107
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113
#define EALREADY 114
#define EINPROGRESS 115
#define EWOULDBLOCK EAGAIN
#define EMFILE  24
#define ENFILE  23

const char* strerror(int errnum);
