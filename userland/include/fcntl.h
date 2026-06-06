#pragma once

#include <sys/types.h>

/*
 * ABI-INVARIANT: open() flag values for EYN-OS.
 *
 * These must match the values decoded by SYSCALL_OPEN in the kernel.
 * O_RDONLY = 0 is the default: any non-write open with flags==0 reads only.
 * Write flags (O_WRONLY, O_RDWR) are accepted by the kernel but EYN-OS VFS
 * currently always opens writeable; the flag is recorded for future use.
 * O_CREAT | O_TRUNC control file-creation / truncation on write paths.
 * O_BINARY is a DOSISH flag accepted but ignored (all I/O is binary on EYN-OS).
 */
#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_ACCMODE 0x0003
#define O_CREAT   0x0040   /* create file if it does not exist */
#define O_EXCL    0x0080
#define O_TRUNC   0x0200   /* truncate file to zero on open   */
#define O_APPEND  0x0400   /* writes always go to end of file */
#define O_NONBLOCK 0x0800  /* non-blocking I/O for supported endpoints */
#ifndef O_CLOEXEC
#define O_CLOEXEC 0x80000
#endif

#ifndef O_NDELAY
#define O_NDELAY O_NONBLOCK
#endif

#ifndef O_NOCTTY
#define O_NOCTTY 0400
#endif

#ifndef O_SYNC
#define O_SYNC 0x101000
#endif
#define O_BINARY  0x0000   /* no-op on EYN-OS (all I/O is binary) */

#ifndef AT_FDCWD
#define AT_FDCWD  (-100)
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif

#define F_GETFL   3
#define F_SETFL   4
#define F_SETFD   2
#define F_SETLK   6
#define F_SETLKW  7

#define F_RDLCK   0
#define F_WRLCK   1
#define F_UNLCK   2

#define FD_CLOEXEC 1

struct flock {
	short l_type;
	short l_whence;
	off_t l_start;
	off_t l_len;
	pid_t l_pid;
};

int open(const char* path, int flags, ...);   /* varargs: optional mode_t mode */
int creat(const char* path, int mode);
int fcntl(int fd, int cmd, ...);
int posix_fallocate(int fd, off_t offset, off_t len);
