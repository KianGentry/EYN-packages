#include <unistd.h>
#include <eynos_syscall.h>

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/uio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <syslog.h>
#include <poll.h>

#ifndef EAI_MEMORY
#define EAI_MEMORY -10
#endif

typedef struct {
    int active;
    int child_active;
    int child_stdin_fd;
    int child_stdout_fd;
    int child_stderr_fd;
    int child_inherit_mode;
    int synthetic_wait_active;
    int synthetic_status;
    pid_t synthetic_pid;
    pid_t resume_pid;
    jmp_buf parent_env;
} eyn_vfork_compat_t;

static eyn_vfork_compat_t g_vfork_compat;
static pid_t g_vfork_next_synthetic_pid = 0x70000000;
static int g_shadow_stdin_fd = 0;
static int g_shadow_stdout_fd = 1;
static int g_shadow_stderr_fd = 2;
static int g_shadow_inherit_mode = 1;

static int eyn_vfork_child_active(void) {
    return g_vfork_compat.active && g_vfork_compat.child_active;
}

int __eyn_vfork_exec_transition(const char* path, char* const argv[], char* const envp[]) {
    (void)envp;

    if (!eyn_vfork_child_active()) return -2;
    if (!path) {
        errno = EINVAL;
        return -1;
    }

    int argc = 0;
    if (argv) {
        while (argv[argc]) argc++;
    }

    eyn_spawn_ex_req_t req;
    req.path = path;
    req.argv = (const char* const*)argv;
    req.argc = argc;
    req.stdin_fd = g_vfork_compat.child_stdin_fd;
    req.stdout_fd = g_vfork_compat.child_stdout_fd;
    req.stderr_fd = g_vfork_compat.child_stderr_fd;
    req.inherit_mode = g_vfork_compat.child_inherit_mode;

    int pid = eyn_sys_spawn_ex(&req);
    if (pid < 0) {
        if (errno == 0) errno = ENOENT;
        return -1;
    }

    g_vfork_compat.resume_pid = pid;
    g_vfork_compat.child_active = 0;
    longjmp(g_vfork_compat.parent_env, 1);
}

void __eyn_vfork_child_exit(int code) {
    if (!eyn_vfork_child_active()) return;

    if (g_vfork_next_synthetic_pid <= 0) g_vfork_next_synthetic_pid = 0x70000000;
    g_vfork_compat.synthetic_pid = g_vfork_next_synthetic_pid++;
    g_vfork_compat.synthetic_status = code;
    g_vfork_compat.synthetic_wait_active = 1;
    g_vfork_compat.resume_pid = g_vfork_compat.synthetic_pid;
    g_vfork_compat.child_active = 0;
    longjmp(g_vfork_compat.parent_env, 1);
}

pid_t vfork(void) {
    if (g_vfork_compat.active) {
        errno = EAGAIN;
        return -1;
    }

    g_vfork_compat.active = 1;
    g_vfork_compat.child_active = 1;
    g_vfork_compat.child_stdin_fd = g_shadow_stdin_fd;
    g_vfork_compat.child_stdout_fd = g_shadow_stdout_fd;
    g_vfork_compat.child_stderr_fd = g_shadow_stderr_fd;
    g_vfork_compat.child_inherit_mode = g_shadow_inherit_mode;

    if (setjmp(g_vfork_compat.parent_env) == 0) {
        return 0;
    }

    g_vfork_compat.active = 0;
    return g_vfork_compat.resume_pid;
}

pid_t fork(void) {
    return vfork();
}

ssize_t write(int fd, const void* buf, size_t len) {
    if (!buf) return -1;
    if (len > 0x7fffffffU) len = 0x7fffffffU;
    return (ssize_t)eyn_syscall3(EYN_SYSCALL_WRITE, fd, buf, (int)len);
}

ssize_t read(int fd, void* buf, size_t len) {
    if (!buf) return -1;
    if (len > 0x7fffffffU) len = 0x7fffffffU;
    return (ssize_t)eyn_syscall3(EYN_SYSCALL_READ, fd, buf, (int)len);
}

ssize_t readv(int fd, const struct iovec* iov, int iovcnt) {
    if (!iov || iovcnt <= 0) {
        errno = EINVAL;
        return -1;
    }
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!iov[i].iov_base || iov[i].iov_len == 0) continue;
        ssize_t rc = read(fd, iov[i].iov_base, iov[i].iov_len);
        if (rc < 0) return (total > 0) ? total : -1;
        total += rc;
        if ((size_t)rc < iov[i].iov_len) break;
    }
    return total;
}

ssize_t writev(int fd, const struct iovec* iov, int iovcnt) {
    if (!iov || iovcnt <= 0) {
        errno = EINVAL;
        return -1;
    }
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!iov[i].iov_base || iov[i].iov_len == 0) continue;
        ssize_t rc = write(fd, iov[i].iov_base, iov[i].iov_len);
        if (rc < 0) return (total > 0) ? total : -1;
        total += rc;
        if ((size_t)rc < iov[i].iov_len) break;
    }
    return total;
}

int close(int fd) {
    if (eyn_vfork_child_active()) {
        if (fd <= 2) {
            errno = ENOSYS;
            return -1;
        }
        g_vfork_compat.child_inherit_mode = 0;
        return 0;
    }
    return eyn_syscall1(EYN_SYSCALL_CLOSE, fd);
}

int isatty(int fd) {
    return (fd >= 0 && fd <= 2) ? 1 : 0;
}


char* ttyname(int fd) {
    static char tty_path[] = "/dev/tty";
    (void)fd;
    return tty_path;
}
int dup(int oldfd) {
    if (eyn_vfork_child_active()) {
        errno = ENOSYS;
        return -1;
    }
    return eyn_syscall1(EYN_SYSCALL_DUP, oldfd);
}

int dup2(int oldfd, int newfd) {
    if (eyn_vfork_child_active()) {
        if (newfd == 0) {
            g_vfork_compat.child_stdin_fd = oldfd;
            return 0;
        }
        if (newfd == 1) {
            g_vfork_compat.child_stdout_fd = oldfd;
            return 1;
        }
        if (newfd == 2) {
            g_vfork_compat.child_stderr_fd = oldfd;
            return 2;
        }
        errno = ENOSYS;
        return -1;
    }
    return eyn_syscall3_iii(EYN_SYSCALL_DUP2, oldfd, newfd, 0);
}

int pipe(int pipefd[2]) {
    if (!pipefd) return -1;
    return eyn_syscall1(EYN_SYSCALL_PIPE, (int)(uintptr_t)pipefd);
}

int mkfifo(const char* path, mode_t mode) {
    (void)mode;
    if (!path) return -1;
    return eyn_syscall1(EYN_SYSCALL_MKFIFO, (int)(uintptr_t)path);
}

int fd_set_inherit(int enabled) {
    if (eyn_vfork_child_active()) {
        int prev = g_vfork_compat.child_inherit_mode;
        g_vfork_compat.child_inherit_mode = enabled ? 1 : 0;
        return prev;
    }
    g_shadow_inherit_mode = enabled ? 1 : 0;
    return eyn_syscall1(EYN_SYSCALL_FD_SET_INHERIT, enabled ? 1 : 0);
}

int fd_set_stdio(int stdin_fd, int stdout_fd, int stderr_fd) {
    if (eyn_vfork_child_active()) {
        g_vfork_compat.child_stdin_fd = stdin_fd;
        g_vfork_compat.child_stdout_fd = stdout_fd;
        g_vfork_compat.child_stderr_fd = stderr_fd;
        return 0;
    }
    g_shadow_stdin_fd = stdin_fd;
    g_shadow_stdout_fd = stdout_fd;
    g_shadow_stderr_fd = stderr_fd;
    return eyn_syscall3_iii(EYN_SYSCALL_FD_SET_STDIO, stdin_fd, stdout_fd, stderr_fd);
}

int fd_set_nonblock(int fd, int enabled) {
    return eyn_syscall3_iii(EYN_SYSCALL_FD_SET_NONBLOCK, fd, enabled ? 1 : 0, 0);
}

int spawn(const char* path, const char* const* argv, int argc) {
    if (!path || argc < 0) return -1;
    return eyn_syscall3_ppi(EYN_SYSCALL_SPAWN,
                            path,
                            (const void*)argv,
                            argc);
}

int spawn_ex(const char* path,
             const char* const* argv,
             int argc,
             int stdin_fd,
             int stdout_fd,
             int stderr_fd,
             int inherit_mode) {
    if (!path || argc < 0) return -1;

    eyn_spawn_ex_req_t req;
    req.path = path;
    req.argv = argv;
    req.argc = argc;
    req.stdin_fd = stdin_fd;
    req.stdout_fd = stdout_fd;
    req.stderr_fd = stderr_fd;
    req.inherit_mode = inherit_mode ? 1 : 0;
    return eyn_sys_spawn_ex(&req);
}

int waitpid(int pid, int* status, int options) {
    if (g_vfork_compat.synthetic_wait_active && (pid == -1 || pid == g_vfork_compat.synthetic_pid)) {
        if (status) *status = g_vfork_compat.synthetic_status;
        g_vfork_compat.synthetic_wait_active = 0;
        return g_vfork_compat.synthetic_pid;
    }
    return eyn_syscall3_iii(EYN_SYSCALL_WAITPID,
                            pid,
                            (int)(uintptr_t)status,
                            options);
}

int wait(int* status) {
    return waitpid(-1, status, 0);
}

int kill(int pid, int sig) {
    return eyn_syscall3_iii(EYN_SYSCALL_KILL, pid, sig, 0);
}

int sigreturn(void) {
    return eyn_syscall0(EYN_SYSCALL_SIGRETURN);
}

/* Install a signal handler for the calling process. */
sighandler_t signal(int sig, sighandler_t handler) {
    (void)eyn_syscall3_iii(EYN_SYSCALL_SIGNAL, sig, (int)(uintptr_t)handler, 0);
    return (sighandler_t)0;
}

int writefile(const char* path, const void* buf, size_t len) {
    if (!path || !buf) return -1;
    if (len > 0x7fffffffU) len = 0x7fffffffU;
    return eyn_syscall3_ppi(EYN_SYSCALL_WRITEFILE, path, buf, (int)len);
}

int mkdir(const char* path, mode_t mode) {
    (void)mode;  /* EYN-OS VFS does not enforce permission bits */
    if (!path) return -1;
    return eyn_syscall1(EYN_SYSCALL_MKDIR, (int)(uintptr_t)path);
}

int unlink(const char* path) {
    if (!path) return -1;
    return eyn_syscall1(EYN_SYSCALL_UNLINK, (int)(uintptr_t)path);
}

int rmdir(const char* path) {
    if (!path) return -1;
    return eyn_syscall1(EYN_SYSCALL_RMDIR, (int)(uintptr_t)path);
}

int link(const char* oldpath, const char* newpath) {
    (void)oldpath;
    (void)newpath;
    errno = ENOSYS;
    return -1;
}

int symlink(const char* target, const char* linkpath) {
    return symlinkat(target, AT_FDCWD, linkpath);
}

int rename(const char* oldpath, const char* newpath) {
    (void)oldpath;
    (void)newpath;
    errno = ENOSYS;
    return -1;
}

int fsync(int fd) {
    (void)fd;
    return 0;
}

int fdatasync(int fd) {
    return fsync(fd);
}

void sync(void) {
}

int mkstemp(char* template_str) {
    static unsigned int counter = 0;
    if (!template_str) {
        errno = EINVAL;
        return -1;
    }

    size_t n = strlen(template_str);
    if (n < 6 || strcmp(template_str + n - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }

    for (unsigned int i = 0; i < 1024; i++) {
        unsigned int v = counter++;
        for (int j = 0; j < 6; j++) {
            template_str[n - 1 - j] = (char)('A' + (v % 26));
            v /= 26;
        }
        int fd = open(template_str, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0) return fd;
    }

    errno = EEXIST;
    return -1;
}

char* mkdtemp(char* template_str) {
    static unsigned int counter = 0;
    if (!template_str) {
        errno = EINVAL;
        return NULL;
    }

    size_t n = strlen(template_str);
    if (n < 6 || strcmp(template_str + n - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return NULL;
    }

    for (unsigned int i = 0; i < 1024; i++) {
        unsigned int v = counter++;
        for (int j = 0; j < 6; j++) {
            template_str[n - 1 - j] = (char)('A' + (v % 26));
            v /= 26;
        }
        if (mkdir(template_str, 0700) == 0) return template_str;
    }

    errno = EEXIST;
    return NULL;
}

#ifdef __chibicc__
void _exit(int code) {
    if (eyn_vfork_child_active()) {
        __eyn_vfork_child_exit(code);
    }
    (void)eyn_syscall1(EYN_SYSCALL_EXIT, code);
    for (;;) {}
}
#else
__attribute__((noreturn)) void _exit(int code) {
    if (eyn_vfork_child_active()) {
        __eyn_vfork_child_exit(code);
    }
    (void)eyn_syscall1(EYN_SYSCALL_EXIT, code);
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
#endif

int getkey(void) {
    return eyn_syscall0(EYN_SYSCALL_GETKEY);
}

int usleep(unsigned int usec) {
    // Cooperative sleep to allow GUI and shell updates.
    (void)eyn_syscall1(EYN_SYSCALL_SLEEP_US, (int)usec);
    return 0;
}

unsigned int sleep(unsigned int seconds) {
    // Best-effort: convert seconds to microseconds.
    unsigned int usec = seconds * 1000000u;
    (void)usleep(usec);
    return 0;
}

unsigned int alarm(unsigned int seconds) {
    (void)seconds;
    return 0;
}

int pause(void) {
    for (;;) {
        usleep(1000000u);
    }
}

long sysconf(int name) {
    if (name == _SC_PAGESIZE) return 4096;
    if (name == _SC_ARG_MAX) return 131072;
    errno = EINVAL;
    return -1;
}

long pathconf(const char* path, int name) {
    (void)path;
    if (name == _PC_NAME_MAX) return 255;
    errno = EINVAL;
    return -1;
}

size_t confstr(int name, char* buf, size_t len) {
    const char* value = NULL;
    if (name == _CS_PATH) value = "/bin:/usr/bin";
    else if (name == _CS_V7_ENV) value = "POSIXLY_CORRECT=1";
    else {
        errno = EINVAL;
        return 0;
    }

    size_t need = strlen(value) + 1;
    if (buf && len > 0) {
        size_t copy = (need <= len) ? need : len;
        memcpy(buf, value, copy);
        if (copy == len) buf[len - 1] = '\0';
    }
    return need;
}

char* getcwd(char* buf, size_t size) {
    if (!buf) {
        if (size == 0) size = 4096;
        buf = (char*)malloc(size);
        if (!buf) return NULL;
    }
    if (size == 0) return NULL;
    if (size > 0x7fffffffU) size = 0x7fffffffU;
    if (eyn_syscall3_pii(EYN_SYSCALL_GETCWD, buf, (int)size, 0) < 0) return NULL;
    return buf;
}

int chdir(const char* path) {
    if (!path) return -1;
    return eyn_syscall1(EYN_SYSCALL_CHDIR, (int)(uintptr_t)path);
}

int fchdir(int fd) {
    (void)fd;
    errno = ENOSYS;
    return -1;
}

int gethostname(char* name, size_t len) {
    static const char kHostname[] = "eyn-os";
    if (!name || len == 0) {
        errno = EINVAL;
        return -1;
    }
    size_t need = sizeof(kHostname);
    if (len < need) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(name, kHostname, need);
    return 0;
}

int sethostname(const char* name, size_t len) {
    (void)name;
    (void)len;
    errno = EPERM;
    return -1;
}

int chroot(const char* path) {
    (void)path;
    errno = ENOSYS;
    return -1;
}

int setsid(void) {
    errno = ENOSYS;
    return -1;
}

pid_t getsid(pid_t pid) {
    (void)pid;
    return 1;
}

int setpgid(pid_t pid, pid_t pgid) {
    (void)pid;
    (void)pgid;
    errno = ENOSYS;
    return -1;
}

int tcsetpgrp(int fd, pid_t pgrp) {
    (void)fd;
    (void)pgrp;
    errno = ENOSYS;
    return -1;
}

int nice(int inc) {
    (void)inc;
    return 0;
}

int setgid(gid_t gid) {
    (void)gid;
    errno = ENOSYS;
    return -1;
}

int setuid(uid_t uid) {
    (void)uid;
    errno = ENOSYS;
    return -1;
}

int getgroups(int size, gid_t list[]) {
    if (size < 0) {
        errno = EINVAL;
        return -1;
    }
    if (size == 0) return 1;
    if (!list) {
        errno = EINVAL;
        return -1;
    }
    list[0] = 0;
    return 1;
}

int getgrouplist(const char* user, gid_t group, gid_t* groups, int* ngroups) {
    (void)user;
    if (!ngroups) {
        errno = EINVAL;
        return -1;
    }
    if (!groups || *ngroups < 1) {
        *ngroups = 1;
        return -1;
    }
    groups[0] = group;
    *ngroups = 1;
    return 1;
}

int initgroups(const char* user, gid_t group) {
    (void)user;
    (void)group;
    errno = ENOSYS;
    return -1;
}

pid_t getpid(void) {
    return 1;
}

pid_t getppid(void) {
    return 1;
}

uid_t getuid(void) {
    return 0;
}

gid_t getgid(void) {
    return 0;
}

gid_t getegid(void) {
    return 0;
}

int chown(const char* path, uid_t owner, gid_t group) {
    (void)path;
    (void)owner;
    (void)group;
    errno = ENOSYS;
    return -1;
}

uid_t geteuid(void) {
    return 0;
}

int openat(int dirfd, const char* path, int flags, ...) {
    (void)flags;
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    if (dirfd != AT_FDCWD) {
        errno = ENOSYS;
        return -1;
    }
    return open(path, flags, 0);
}

int mkdirat(int dirfd, const char* path, mode_t mode) {
    if (dirfd != AT_FDCWD) {
        errno = ENOSYS;
        return -1;
    }
    return mkdir(path, mode);
}

int fstatat(int dirfd, const char* path, struct stat* st, int flags) {
    (void)flags;
    if (!path || !st) {
        errno = EINVAL;
        return -1;
    }
    if (dirfd != AT_FDCWD) {
        errno = ENOSYS;
        return -1;
    }
    return stat(path, st);
}

int faccessat(int dirfd, const char* path, int mode, int flags) {
    (void)flags;
    if (dirfd != AT_FDCWD) {
        errno = ENOSYS;
        return -1;
    }
    return access(path, mode);
}

int linkat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath, int flags) {
    (void)flags;
    if (olddirfd != AT_FDCWD || newdirfd != AT_FDCWD) {
        errno = ENOSYS;
        return -1;
    }
    return link(oldpath, newpath);
}

int symlinkat(const char* target, int newdirfd, const char* linkpath) {
    (void)target;
    (void)newdirfd;
    (void)linkpath;
    errno = ENOSYS;
    return -1;
}

int mknodat(int dirfd, const char* path, mode_t mode, dev_t dev) {
    if (dirfd != AT_FDCWD) {
        errno = ENOSYS;
        return -1;
    }
    return mknod(path, mode, dev);
}

int futimens(int fd, const struct timespec times[2]) {
    (void)fd;
    (void)times;
    return 0;
}

int lchown(const char* path, uid_t owner, gid_t group) {
    return chown(path, owner, group);
}

int fchownat(int dirfd, const char* path, uid_t owner, gid_t group, int flags) {
    (void)flags;
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    if (dirfd != AT_FDCWD) {
        errno = ENOSYS;
        return -1;
    }
    return chown(path, owner, group);
}

int unlinkat(int dirfd, const char* path, int flags) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    if (dirfd != AT_FDCWD) {
        errno = ENOSYS;
        return -1;
    }
    if (flags & AT_REMOVEDIR) return rmdir(path);
    return unlink(path);
}

ssize_t readlinkat(int dirfd, const char* path, char* buf, size_t bufsize) {
    (void)dirfd;
    (void)path;
    (void)buf;
    (void)bufsize;
    errno = ENOSYS;
    return -1;
}

ssize_t readlink(const char* path, char* buf, size_t bufsize) {
    return readlinkat(AT_FDCWD, path, buf, bufsize);
}

int eynfs_stream_begin(const char* path) {
    if (!path) return -1;
    return eyn_syscall1(EYN_SYSCALL_EYNFS_STREAM_BEGIN, (int)(uintptr_t)path);
}

ssize_t eynfs_stream_write(int handle, const void* buf, size_t len) {
    if (!buf) return -1;
    if (len > 0x7fffffffU) len = 0x7fffffffU;
    return (ssize_t)eyn_syscall3(EYN_SYSCALL_EYNFS_STREAM_WRITE, handle, buf, (int)len);
}

int eynfs_stream_end(int handle) {
    return eyn_syscall1(EYN_SYSCALL_EYNFS_STREAM_END, handle);
}

/*
 * lseek() -- reposition an open file descriptor's read offset.
 *
 * Wraps SYSCALL_LSEEK (110).  whence values match POSIX:
 *   SEEK_SET (0): offset from start of file
 *   SEEK_CUR (1): offset from current position
 *   SEEK_END (2): offset from end of file
 *
 * Returns the new offset on success, or -1 on error.
 */
long lseek(int fd, long offset, int whence) {
    return (long)eyn_syscall3_iii(
        EYN_SYSCALL_LSEEK,
        fd,
        (int)offset,
        whence
    );
}

ssize_t readahead(int fd, off64_t offset, size_t count) {
    (void)fd;
    (void)offset;
    (void)count;
    errno = ENOSYS;
    return -1;
}

/*
 * access() -- check accessibility of a file path.
 *
 * EYN-OS has no permission model; any path that exists is considered
 * accessible.  We attempt to open the file read-only; if it succeeds the
 * path is accessible (return 0), otherwise it is not (return -1).
 * The mode argument (F_OK / R_OK / X_OK) is accepted but ignored since
 * all checks reduce to "does this path exist".
 */
int access(const char* path, int mode) {
    (void)mode;
    if (!path) return -1;
    int fd = eyn_syscall1(EYN_SYSCALL_OPEN, (int)(uintptr_t)path);
    if (fd < 0) return -1;
    eyn_syscall1(EYN_SYSCALL_CLOSE, fd);
    return 0;
}

int ftruncate(int fd, off_t length) {
    (void)fd;
    (void)length;
    errno = ENOSYS;
    return -1;
}

void* mmap(void* addr, size_t length, int prot, int flags, int fd, long offset) {
    uintptr_t ret;
    int off = (int)offset;

    if (length == 0) return (void*)-1;

    __asm__ __volatile__(
        "push %%ebp\n\t"
        "movl %7, %%ebp\n\t"
        "int $0x80\n\t"
        "pop %%ebp\n\t"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_MMAP), "b"(addr), "c"(length), "d"(prot), "S"(flags), "D"(fd), "m"(off)
        : "memory"
    );

    if (ret == (uintptr_t)-1) return (void*)-1;
    return (void*)(uintptr_t)ret;
}

int munmap(void* addr, size_t length) {
    return eyn_syscall3_iii(EYN_SYSCALL_MUNMAP, (int)(uintptr_t)addr, (int)length, 0);
}

int uname(struct utsname* buf) {
    if (!buf) return -1;

    (void)strncpy(buf->sysname, "EYN-OS", sizeof(buf->sysname));
    buf->sysname[sizeof(buf->sysname) - 1] = '\0';

    (void)strncpy(buf->nodename, "eyn-host", sizeof(buf->nodename));
    buf->nodename[sizeof(buf->nodename) - 1] = '\0';

    (void)strncpy(buf->release, "0", sizeof(buf->release));
    buf->release[sizeof(buf->release) - 1] = '\0';

    (void)strncpy(buf->version, "EYN-OS", sizeof(buf->version));
    buf->version[sizeof(buf->version) - 1] = '\0';

    (void)strncpy(buf->machine, "i386", sizeof(buf->machine));
    buf->machine[sizeof(buf->machine) - 1] = '\0';

    return 0;
}

int socket(int domain, int type, int protocol) {
    (void)domain;
    (void)type;
    (void)protocol;
    errno = ENOSYS;
    return -1;
}

int mlock(const void* addr, size_t len) {
    (void)addr;
    (void)len;
    return 0;
}

int munlock(const void* addr, size_t len) {
    (void)addr;
    (void)len;
    return 0;
}

int mlockall(int flags) {
    (void)flags;
    return 0;
}

int bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    (void)sockfd;
    (void)addr;
    (void)addrlen;
    errno = ENOSYS;
    return -1;
}

int connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    (void)sockfd;
    (void)addr;
    (void)addrlen;
    errno = ENOSYS;
    return -1;
}

int listen(int sockfd, int backlog) {
    (void)sockfd;
    (void)backlog;
    errno = ENOSYS;
    return -1;
}

int accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    (void)sockfd;
    (void)addr;
    (void)addrlen;
    errno = ENOSYS;
    return -1;
}

ssize_t send(int sockfd, const void* buf, size_t len, int flags) {
    (void)sockfd;
    (void)buf;
    (void)len;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

ssize_t recv(int sockfd, void* buf, size_t len, int flags) {
    (void)sockfd;
    (void)buf;
    (void)len;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

ssize_t sendmsg(int sockfd, const struct msghdr* msg, int flags) {
    (void)sockfd;
    (void)msg;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

ssize_t recvmsg(int sockfd, struct msghdr* msg, int flags) {
    (void)sockfd;
    (void)msg;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

ssize_t sendto(int sockfd, const void* buf, size_t len, int flags, const struct sockaddr* dest_addr, socklen_t addrlen) {
    (void)sockfd;
    (void)buf;
    (void)len;
    (void)flags;
    (void)dest_addr;
    (void)addrlen;
    errno = ENOSYS;
    return -1;
}

ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags, struct sockaddr* src_addr, socklen_t* addrlen) {
    (void)sockfd;
    (void)buf;
    (void)len;
    (void)flags;
    (void)src_addr;
    (void)addrlen;
    errno = ENOSYS;
    return -1;
}

int setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen) {
    (void)sockfd;
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    errno = ENOSYS;
    return -1;
}

int getsockopt(int sockfd, int level, int optname, void* optval, socklen_t* optlen) {
    (void)sockfd;
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    errno = ENOSYS;
    return -1;
}

int getsockname(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    (void)sockfd;
    (void)addr;
    (void)addrlen;
    errno = ENOSYS;
    return -1;
}

int getrlimit(int resource, struct rlimit* rlim) {
    (void)resource;
    if (!rlim) {
        errno = EINVAL;
        return -1;
    }
    rlim->rlim_cur = RLIM_INFINITY;
    rlim->rlim_max = RLIM_INFINITY;
    return 0;
}

int setrlimit(int resource, const struct rlimit* rlim) {
    (void)resource;
    (void)rlim;
    return 0;
}

int getpriority(int which, id_t who) {
    (void)which;
    (void)who;
    return 0;
}

int setpriority(int which, id_t who, int prio) {
    (void)which;
    (void)who;
    (void)prio;
    return 0;
}

pid_t wait4(pid_t pid, int* status, int options, struct rusage* rusage) {
    if (rusage) memset(rusage, 0, sizeof(*rusage));
    return waitpid(pid, status, options);
}

int getpeername(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    (void)sockfd;
    (void)addr;
    (void)addrlen;
    errno = ENOSYS;
    return -1;
}

int shutdown(int sockfd, int how) {
    (void)sockfd;
    (void)how;
    errno = ENOSYS;
    return -1;
}

static struct passwd g_pwd_root = {"root", "x", 0, 0, "root", "/", "/bin/sh"};
static struct group g_grp_root = {"root", "x", 0, NULL};
static struct spwd g_spwd_root = {"root", "*", 0, 0, 99999, 7, 0, 0, 0};

struct passwd* getpwnam(const char* name) {
    if (name && strcmp(name, "root") == 0) return &g_pwd_root;
    return NULL;
}

struct passwd* getpwuid(uid_t uid) {
    if (uid == 0) return &g_pwd_root;
    return NULL;
}

int getpwnam_r(const char* name, struct passwd* pwd, char* buf, size_t buflen, struct passwd** result) {
    (void)buf;
    (void)buflen;
    if (!pwd || !result) return EINVAL;
    if (!name || strcmp(name, "root") != 0) {
        *result = NULL;
        return 0;
    }
    *pwd = g_pwd_root;
    *result = pwd;
    return 0;
}

int getpwuid_r(uid_t uid, struct passwd* pwd, char* buf, size_t buflen, struct passwd** result) {
    (void)buf;
    (void)buflen;
    if (!pwd || !result) return EINVAL;
    if (uid != 0) {
        *result = NULL;
        return 0;
    }
    *pwd = g_pwd_root;
    *result = pwd;
    return 0;
}

struct group* getgrnam(const char* name) {
    if (name && strcmp(name, "root") == 0) return &g_grp_root;
    return NULL;
}

struct group* getgrgid(gid_t gid) {
    if (gid == 0) return &g_grp_root;
    return NULL;
}

int getgrnam_r(const char* name, struct group* grp, char* buf, size_t buflen, struct group** result) {
    (void)buf;
    (void)buflen;
    if (!grp || !result) return EINVAL;
    if (!name || strcmp(name, "root") != 0) {
        *result = NULL;
        return 0;
    }
    *grp = g_grp_root;
    *result = grp;
    return 0;
}

int getgrgid_r(gid_t gid, struct group* grp, char* buf, size_t buflen, struct group** result) {
    (void)buf;
    (void)buflen;
    if (!grp || !result) return EINVAL;
    if (gid != 0) {
        *result = NULL;
        return 0;
    }
    *grp = g_grp_root;
    *result = grp;
    return 0;
}

struct spwd* getspnam(const char* name) {
    if (name && strcmp(name, "root") == 0) return &g_spwd_root;
    return NULL;
}

static const char* g_openlog_ident = "toybox";

void openlog(const char* ident, int option, int facility) {
    (void)option;
    (void)facility;
    if (ident && *ident) g_openlog_ident = ident;
}

void closelog(void) {
}

void vsyslog(int priority, const char* format, va_list ap) {
    (void)priority;
    fprintf(stderr, "%s: ", g_openlog_ident);
    vfprintf(stderr, format, ap);
    fprintf(stderr, "\n");
}

void syslog(int priority, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    vsyslog(priority, format, ap);
    va_end(ap);
}

int ioctl(int fd, unsigned long request, ...) {
    (void)fd;
    (void)request;
    errno = ENOSYS;
    return -1;
}

int poll(struct pollfd* fds, nfds_t nfds, int timeout_ms) {
    (void)fds;
    (void)nfds;
    if (timeout_ms > 0) usleep((unsigned int)timeout_ms * 1000u);
    return 0;
}

int fnmatch(const char* pattern, const char* string, int flags) {
    (void)flags;
    if (!pattern || !string) return 1;
    return strcmp(pattern, string) ? 1 : 0;
}

typedef struct {
    int re_magic;
} regex_t;

typedef struct {
    int rm_so;
    int rm_eo;
} regmatch_t;

int regcomp(regex_t* preg, const char* regex, int cflags) {
    (void)regex;
    (void)cflags;
    if (!preg) return EINVAL;
    preg->re_magic = 1;
    return 0;
}

int regexec(const regex_t* preg, const char* string, size_t nmatch, regmatch_t pmatch[], int eflags) {
    (void)preg;
    (void)string;
    (void)nmatch;
    (void)pmatch;
    (void)eflags;
    return 1;
}

size_t regerror(int errcode, const regex_t* preg, char* errbuf, size_t errbuf_size) {
    (void)errcode;
    (void)preg;
    const char* msg = "regex unsupported";
    size_t need = strlen(msg) + 1;
    if (errbuf && errbuf_size) {
        size_t n = need <= errbuf_size ? need : errbuf_size;
        memcpy(errbuf, msg, n);
        errbuf[n - 1] = '\0';
    }
    return need;
}

void regfree(regex_t* preg) {
    (void)preg;
}

int getentropy(void* buffer, size_t length) {
    unsigned char* p = (unsigned char*)buffer;
    if (!p) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < length; i++) p[i] = (unsigned char)((i * 37u) + 17u);
    return 0;
}

struct statvfs64 {
    unsigned long f_bsize;
    unsigned long f_frsize;
    unsigned long long f_blocks;
    unsigned long long f_bfree;
    unsigned long long f_bavail;
};

int statvfs64(const char* path, struct statvfs64* buf) {
    (void)path;
    if (!buf) {
        errno = EINVAL;
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    buf->f_bsize = 4096;
    buf->f_frsize = 4096;
    return 0;
}

struct statfs64 {
    long f_type;
    long f_bsize;
    unsigned long long f_blocks;
    unsigned long long f_bfree;
    unsigned long long f_bavail;
};

int statfs64(const char* path, struct statfs64* buf) {
    (void)path;
    if (!buf) {
        errno = EINVAL;
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    buf->f_bsize = 4096;
    return 0;
}

int inotify_init(void) { errno = ENOSYS; return -1; }
int inotify_add_watch(int fd, const char* pathname, uint32_t mask) {
    (void)fd;
    (void)pathname;
    (void)mask;
    errno = ENOSYS;
    return -1;
}
int inotify_rm_watch(int fd, int wd) {
    (void)fd;
    (void)wd;
    errno = ENOSYS;
    return -1;
}

int getxattr(const char* path, const char* name, void* value, size_t size) {
    (void)path; (void)name; (void)value; (void)size; errno = ENOSYS; return -1;
}
int lgetxattr(const char* path, const char* name, void* value, size_t size) {
    (void)path; (void)name; (void)value; (void)size; errno = ENOSYS; return -1;
}
int fgetxattr(int fd, const char* name, void* value, size_t size) {
    (void)fd; (void)name; (void)value; (void)size; errno = ENOSYS; return -1;
}
int listxattr(const char* path, char* list, size_t size) {
    (void)path; (void)list; (void)size; errno = ENOSYS; return -1;
}
int llistxattr(const char* path, char* list, size_t size) {
    (void)path; (void)list; (void)size; errno = ENOSYS; return -1;
}
int flistxattr(int fd, char* list, size_t size) {
    (void)fd; (void)list; (void)size; errno = ENOSYS; return -1;
}
int setxattr(const char* path, const char* name, const void* value, size_t size, int flags) {
    (void)path; (void)name; (void)value; (void)size; (void)flags; errno = ENOSYS; return -1;
}
int lsetxattr(const char* path, const char* name, const void* value, size_t size, int flags) {
    (void)path; (void)name; (void)value; (void)size; (void)flags; errno = ENOSYS; return -1;
}
int fsetxattr(int fd, const char* name, const void* value, size_t size, int flags) {
    (void)fd; (void)name; (void)value; (void)size; (void)flags; errno = ENOSYS; return -1;
}
int removexattr(const char* path, const char* name) {
    (void)path; (void)name; errno = ENOSYS; return -1;
}
int lremovexattr(const char* path, const char* name) {
    (void)path; (void)name; errno = ENOSYS; return -1;
}

int mount(const char* source, const char* target, const char* filesystemtype, unsigned long mountflags, const void* data) {
    (void)source; (void)target; (void)filesystemtype; (void)mountflags; (void)data; errno = ENOSYS; return -1;
}
int umount2(const char* target, int flags) {
    (void)target; (void)flags; errno = ENOSYS; return -1;
}
int swapon(const char* path, int swapflags) {
    (void)path; (void)swapflags; errno = ENOSYS; return -1;
}
int swapoff(const char* path) {
    (void)path; errno = ENOSYS; return -1;
}
int reboot(int cmd) {
    (void)cmd; errno = ENOSYS; return -1;
}
int klogctl(int type, char* bufp, int len) {
    (void)type; (void)bufp; (void)len; errno = ENOSYS; return -1;
}

struct sysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long procs;
};

int sysinfo(struct sysinfo* info) {
    if (!info) {
        errno = EINVAL;
        return -1;
    }
    memset(info, 0, sizeof(*info));
    return 0;
}

int personality(unsigned long persona) {
    (void)persona;
    return 0;
}

int prlimit(pid_t pid, int resource, const struct rlimit* new_limit, struct rlimit* old_limit) {
    (void)pid;
    if (old_limit) {
        old_limit->rlim_cur = RLIM_INFINITY;
        old_limit->rlim_max = RLIM_INFINITY;
    }
    if (new_limit) (void)resource;
    return 0;
}

int flock(int fd, int operation) {
    (void)fd;
    (void)operation;
    errno = ENOSYS;
    return -1;
}

int tcsendbreak(int fd, int duration) {
    (void)fd;
    (void)duration;
    return 0;
}

int dn_expand(const unsigned char* msg, const unsigned char* eomorig, const unsigned char* comp_dn, char* exp_dn, int length) {
    (void)msg;
    (void)eomorig;
    (void)comp_dn;
    if (!exp_dn || length <= 0) return -1;
    exp_dn[0] = '\0';
    return 1;
}

int res_mkquery(int op, const char* dname, int class, int type, const unsigned char* data,
                int datalen, const unsigned char* newrr, unsigned char* buf, int buflen) {
    (void)op; (void)dname; (void)class; (void)type; (void)data; (void)datalen; (void)newrr;
    if (!buf || buflen <= 0) return -1;
    return 0;
}

struct in_addr {
    uint32_t s_addr;
};

char* inet_ntoa(struct in_addr in) {
    static char out[16];
    unsigned int a = (in.s_addr >> 24) & 0xffu;
    unsigned int b = (in.s_addr >> 16) & 0xffu;
    unsigned int c = (in.s_addr >> 8) & 0xffu;
    unsigned int d = in.s_addr & 0xffu;
    snprintf(out, sizeof(out), "%u.%u.%u.%u", a, b, c, d);
    return out;
}

uint32_t inet_addr(const char* cp) {
    unsigned int a, b, c, d;
    if (!cp) return (uint32_t)-1;
    if (sscanf(cp, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return (uint32_t)-1;
    return (a << 24) | (b << 16) | (c << 8) | d;
}

const char* inet_ntop(int af, const void* src, char* dst, socklen_t size) {
    (void)af;
    if (!src || !dst || size < 16) {
        errno = EINVAL;
        return NULL;
    }
    uint32_t v = *(const uint32_t*)src;
    snprintf(dst, size, "%u.%u.%u.%u", (v >> 24) & 0xffu, (v >> 16) & 0xffu, (v >> 8) & 0xffu, v & 0xffu);
    return dst;
}

int inet_pton(int af, const char* src, void* dst) {
    (void)af;
    if (!src || !dst) return 0;
    uint32_t v = inet_addr(src);
    if (v == (uint32_t)-1) return 0;
    *(uint32_t*)dst = v;
    return 1;
}

struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    size_t ai_addrlen;
    struct sockaddr* ai_addr;
    char* ai_canonname;
    struct addrinfo* ai_next;
};

int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res) {
    (void)service;
    (void)hints;
    if (!res) return EINVAL;
    *res = NULL;
    if (!node) return EINVAL;

    struct addrinfo* ai = (struct addrinfo*)calloc(1, sizeof(*ai));
    struct sockaddr_in* sa = (struct sockaddr_in*)calloc(1, sizeof(*sa));
    if (!ai || !sa) {
        free(ai);
        free(sa);
        return EAI_MEMORY;
    }
    sa->sin_family = AF_INET;
    sa->sin_port = 0;
    sa->sin_addr.s_addr = inet_addr(node);
    ai->ai_family = AF_INET;
    ai->ai_socktype = SOCK_STREAM;
    ai->ai_protocol = 0;
    ai->ai_addrlen = sizeof(*sa);
    ai->ai_addr = (struct sockaddr*)sa;
    ai->ai_next = NULL;
    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo* res) {
    while (res) {
        struct addrinfo* next = res->ai_next;
        free(res->ai_addr);
        free(res);
        res = next;
    }
}

const char* gai_strerror(int errcode) {
    (void)errcode;
    return "address resolution error";
}

int getnameinfo(const struct sockaddr* addr, socklen_t addrlen, char* host, socklen_t hostlen,
                char* serv, socklen_t servlen, int flags) {
    (void)addr;
    (void)addrlen;
    (void)flags;
    if (host && hostlen) {
        strncpy(host, "localhost", hostlen);
        host[hostlen - 1] = '\0';
    }
    if (serv && servlen) {
        strncpy(serv, "0", servlen);
        serv[servlen - 1] = '\0';
    }
    return 0;
}

struct hostent {
    char* h_name;
    char** h_aliases;
    int h_addrtype;
    int h_length;
    char** h_addr_list;
};

struct hostent* gethostbyname(const char* name) {
    static struct hostent ent;
    static char* addr_list[2];
    static uint32_t addr = 0x7f000001u;
    addr_list[0] = (char*)&addr;
    addr_list[1] = NULL;
    ent.h_name = (char*)(name ? name : "localhost");
    ent.h_aliases = NULL;
    ent.h_addrtype = AF_INET;
    ent.h_length = 4;
    ent.h_addr_list = addr_list;
    return &ent;
}

int h_errno;
int* __h_errno_location(void) {
    return &h_errno;
}

const char* hstrerror(int err) {
    (void)err;
    return "host error";
}

struct servent {
    char* s_name;
    char** s_aliases;
    int s_port;
    char* s_proto;
};

struct servent* getservbyport(int port, const char* proto) {
    static struct servent s;
    (void)proto;
    s.s_name = "unknown";
    s.s_aliases = NULL;
    s.s_port = port;
    s.s_proto = "tcp";
    return &s;
}

struct ifaddrs {
    struct ifaddrs* ifa_next;
    char* ifa_name;
    unsigned int ifa_flags;
    struct sockaddr* ifa_addr;
    struct sockaddr* ifa_netmask;
    struct sockaddr* ifa_ifu;
    void* ifa_data;
};

int getifaddrs(struct ifaddrs** ifap) {
    if (!ifap) {
        errno = EINVAL;
        return -1;
    }
    *ifap = NULL;
    return 0;
}

void freeifaddrs(struct ifaddrs* ifa) {
    (void)ifa;
}

typedef void* locale_t;
char* setlocale(int category, const char* locale) {
    (void)category;
    (void)locale;
    return "C";
}

char* nl_langinfo(int item) {
    (void)item;
    return "";
}

locale_t newlocale(int category_mask, const char* locale, locale_t base) {
    (void)category_mask;
    (void)locale;
    return base;
}

locale_t uselocale(locale_t newloc) {
    return newloc;
}

struct utmpx {
    short ut_type;
    pid_t ut_pid;
    char ut_line[32];
    char ut_id[4];
    char ut_user[32];
    char ut_host[256];
};

void setutxent(void) {}
void endutxent(void) {}
struct utmpx* getutxent(void) { return NULL; }

char* optarg;
int optind = 1;
int opterr = 1;
int optopt;

struct option {
    const char* name;
    int has_arg;
    int* flag;
    int val;
};

int getopt_long(int argc, char* const argv[], const char* optstring,
                const struct option* longopts, int* longindex) {
    (void)argc;
    (void)argv;
    (void)optstring;
    (void)longopts;
    (void)longindex;
    return -1;
}

int getopt_long_only(int argc, char* const argv[], const char* optstring,
                     const struct option* longopts, int* longindex) {
    return getopt_long(argc, argv, optstring, longopts, longindex);
}

typedef void* iconv_t;
iconv_t iconv_open(const char* tocode, const char* fromcode) {
    (void)tocode;
    (void)fromcode;
    return (iconv_t)1;
}

size_t iconv(iconv_t cd, char** inbuf, size_t* inbytesleft, char** outbuf, size_t* outbytesleft) {
    (void)cd;
    if (!inbuf || !inbytesleft || !outbuf || !outbytesleft) return (size_t)-1;
    size_t n = (*inbytesleft < *outbytesleft) ? *inbytesleft : *outbytesleft;
    if (n) memcpy(*outbuf, *inbuf, n);
    *inbuf += n;
    *outbuf += n;
    *inbytesleft -= n;
    *outbytesleft -= n;
    return *inbytesleft ? (size_t)-1 : 0;
}

struct timex {
    unsigned int modes;
};

int adjtimex(struct timex* txc_p) {
    (void)txc_p;
    errno = ENOSYS;
    return -1;
}

long syscall(long number, ...) {
    (void)number;
    errno = ENOSYS;
    return -1;
}
