#include <fcntl.h>
#include <eynos_syscall.h>
#include <stdarg.h>
#include <errno.h>

int open(const char* path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (!path) return -1;
    return eyn_syscall3_pii(EYN_SYSCALL_OPEN, path, flags, mode);
}

int creat(const char* path, int mode) {
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    int arg = 0;

    va_start(ap, cmd);
    if (cmd == F_SETFL || cmd == F_SETFD || cmd == F_SETLK || cmd == F_SETLKW) {
        arg = va_arg(ap, int);
    }
    va_end(ap);

    if (cmd == F_SETFD) {
        (void)fd;
        (void)arg;
        return 0;
    }

    if (cmd == F_SETLK || cmd == F_SETLKW) {
        (void)fd;
        (void)arg;
        return 0;
    }

    if (cmd == F_GETFL || cmd == F_SETFL) {
#ifdef EYN_SYSCALL_FCNTL
        return eyn_syscall3_iii(EYN_SYSCALL_FCNTL, fd, cmd, arg);
#else
        (void)fd;
        (void)arg;
        return (cmd == F_GETFL) ? O_RDONLY : 0;
#endif
    }

    return -1;
}

int posix_fallocate(int fd, off_t offset, off_t len) {
    (void)fd;
    (void)offset;
    (void)len;
    return ENOSYS;
}
