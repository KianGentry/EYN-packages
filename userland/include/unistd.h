#pragma once

#include <stddef.h>
#include <sys/types.h>  /* mode_t, off_t */
#include <sys/mman.h>
#include <time.h>
#include <signal.h>

// chibicc doesn't implement GNU __attribute__ yet.
#ifdef __chibicc__
#define EYN_ATTR_NORETURN
#else
#define EYN_ATTR_NORETURN __attribute__((noreturn))
#endif

// EYN-OS supports fd=0 (stdin), fd=1 (stdout) today.
ssize_t write(int fd, const void* buf, size_t len);
ssize_t read(int fd, void* buf, size_t len);

int close(int fd);
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int isatty(int fd);

// IPC primitives
int pipe(int pipefd[2]);
int mkfifo(const char* path, mode_t mode);

// Explicit FD inheritance controls for spawn/run workflows.
int fd_set_inherit(int enabled);
int fd_set_stdio(int stdin_fd, int stdout_fd, int stderr_fd);
int fd_set_nonblock(int fd, int enabled);

/* Restricted NOMMU-style compat layer: child may only exec or _exit. */
pid_t vfork(void);
pid_t fork(void);

#define WNOHANG 1
int spawn(const char* path, const char* const* argv, int argc);
int spawn_ex(const char* path,
			 const char* const* argv,
			 int argc,
			 int stdin_fd,
			 int stdout_fd,
			 int stderr_fd,
			 int inherit_mode);
int wait(int* status);
int waitpid(int pid, int* status, int options);

/* Signal primitives */
int kill(int pid, int sig);
int sigreturn(void);

// Create/overwrite a file with given contents.
int writefile(const char* path, const void* buf, size_t len);

// Filesystem mutation helpers.
int mkdir(const char* path, mode_t mode);  /* mode is accepted but ignored on EYN-OS */
int unlink(const char* path);
int rmdir(const char* path);
int link(const char* oldpath, const char* newpath);
int symlink(const char* target, const char* linkpath);
int rename(const char* oldpath, const char* newpath);
int fsync(int fd);
int fdatasync(int fd);
void sync(void);

/* access() checks existence (F_OK) or basic read/exec permission (R_OK/X_OK).
 * EYN-OS has no permission bits, so any accessible path returns 0. */
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1
int access(const char* path, int mode);
int ftruncate(int fd, off_t length);

void _exit(int code) EYN_ATTR_NORETURN;

// Non-blocking single-key read. Returns 0 if none available.
int getkey(void);

// Sleep helpers (cooperative).
int usleep(unsigned int usec);
unsigned int sleep(unsigned int seconds);
unsigned int alarm(unsigned int seconds);
int pause(void);

#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE 30
#endif
#ifndef _SC_ADVISORY_INFO
#define _SC_ADVISORY_INFO 1000
#define _SC_BARRIERS 1001
#define _SC_ASYNCHRONOUS_IO 1002
#define _SC_CLOCK_SELECTION 1003
#define _SC_CPUTIME 1004
#define _SC_FSYNC 1005
#define _SC_IPV6 1006
#define _SC_JOB_CONTROL 1007
#define _SC_MAPPED_FILES 1008
#define _SC_MEMLOCK 1009
#define _SC_MEMLOCK_RANGE 1010
#define _SC_MEMORY_PROTECTION 1011
#define _SC_MESSAGE_PASSING 1012
#define _SC_MONOTONIC_CLOCK 1013
#define _SC_PRIORITY_SCHEDULING 1014
#define _SC_RAW_SOCKETS 1015
#define _SC_READER_WRITER_LOCKS 1016
#define _SC_REALTIME_SIGNALS 1017
#define _SC_REGEXP 1018
#define _SC_SAVED_IDS 1019
#define _SC_SEMAPHORES 1020
#define _SC_SHARED_MEMORY_OBJECTS 1021
#define _SC_SHELL 1022
#define _SC_SPAWN 1023
#define _SC_SPIN_LOCKS 1024
#define _SC_SPORADIC_SERVER 1025
#define _SC_SS_REPL_MAX 1026
#define _SC_SYNCHRONIZED_IO 1027
#define _SC_THREAD_ATTR_STACKADDR 1028
#define _SC_THREAD_ATTR_STACKSIZE 1029
#define _SC_THREAD_CPUTIME 1030
#define _SC_THREAD_PRIO_INHERIT 1031
#define _SC_THREAD_PRIO_PROTECT 1032
#define _SC_THREAD_PRIORITY_SCHEDULING 1033
#define _SC_THREAD_PROCESS_SHARED 1034
#define _SC_THREAD_ROBUST_PRIO_INHERIT 1035
#define _SC_THREAD_ROBUST_PRIO_PROTECT 1036
#define _SC_THREAD_SAFE_FUNCTIONS 1037
#define _SC_THREAD_SPORADIC_SERVER 1038
#define _SC_THREADS 1039
#define _SC_TIMEOUTS 1040
#define _SC_TIMERS 1041
#define _SC_TRACE 1042
#define _SC_TRACE_EVENT_FILTER 1043
#define _SC_TRACE_EVENT_NAME_MAX 1044
#define _SC_TRACE_INHERIT 1045
#define _SC_TRACE_LOG 1046
#define _SC_TRACE_NAME_MAX 1047
#define _SC_TRACE_SYS_MAX 1048
#define _SC_TRACE_USER_EVENT_MAX 1049
#define _SC_TYPED_MEMORY_OBJECTS 1050
#define _SC_VERSION 1051
#define _SC_V7_ILP32_OFF32 1052
#define _SC_V7_ILP32_OFFBIG 1053
#define _SC_V7_LP64_OFF64 1054
#define _SC_V7_LPBIG_OFFBIG 1055
#define _SC_2_C_BIND 1056
#define _SC_2_C_DEV 1057
#define _SC_2_CHAR_TERM 1058
#define _SC_2_FORT_DEV 1059
#define _SC_2_FORT_RUN 1060
#define _SC_2_LOCALEDEF 1061
#define _SC_2_PBS 1062
#define _SC_2_PBS_ACCOUNTING 1063
#define _SC_2_PBS_CHECKPOINT 1064
#define _SC_2_PBS_LOCATE 1065
#define _SC_2_PBS_MESSAGE 1066
#define _SC_2_PBS_TRACK 1067
#define _SC_2_SW_DEV 1068
#define _SC_2_UPE 1069
#define _SC_2_VERSION 1070
#define _SC_XOPEN_CRYPT 1071
#define _SC_XOPEN_ENH_I18N 1072
#define _SC_XOPEN_REALTIME 1073
#define _SC_XOPEN_REALTIME_THREADS 1074
#define _SC_XOPEN_SHM 1075
#define _SC_XOPEN_STREAMS 1076
#define _SC_XOPEN_UNIX 1077
#define _SC_XOPEN_VERSION 1078
#define _SC_AIO_LISTIO_MAX 1079
#define _SC_AIO_MAX 1080
#define _SC_AIO_PRIO_DELTA_MAX 1081
#define _SC_ATEXIT_MAX 1082
#define _SC_BC_BASE_MAX 1083
#define _SC_BC_DIM_MAX 1084
#define _SC_BC_SCALE_MAX 1085
#define _SC_BC_STRING_MAX 1086
#define _SC_CHILD_MAX 1087
#define _SC_CLK_TCK 1088
#define _SC_COLL_WEIGHTS_MAX 1089
#define _SC_DELAYTIMER_MAX 1090
#define _SC_EXPR_NEST_MAX 1091
#define _SC_HOST_NAME_MAX 1092
#define _SC_IOV_MAX 1093
#define _SC_LINE_MAX 1094
#define _SC_LOGIN_NAME_MAX 1095
#define _SC_NGROUPS_MAX 1096
#define _SC_MQ_OPEN_MAX 1097
#define _SC_MQ_PRIO_MAX 1098
#define _SC_NPROCESSORS_CONF 1099
#define _SC_NPROCESSORS_ONLN 1100
#define _SC_OPEN_MAX 1101
#define _SC_RE_DUP_MAX 1102
#define _SC_RTSIG_MAX 1103
#define _SC_SEM_NSEMS_MAX 1104
#define _SC_SEM_VALUE_MAX 1105
#define _SC_SIGQUEUE_MAX 1106
#define _SC_STREAM_MAX 1107
#define _SC_SYMLOOP_MAX 1108
#define _SC_TIMER_MAX 1109
#define _SC_TTY_NAME_MAX 1110
#define _SC_TZNAME_MAX 1111
#define _SC_UIO_MAXIOV 1112
#define _SC_AVPHYS_PAGES 1113
#define _SC_PHYS_PAGES 1114
#define _SC_THREAD_DESTRUCTOR_ITERATIONS 1115
#define _SC_THREAD_KEYS_MAX 1116
#define _SC_THREAD_STACK_MIN 1117
#define _SC_THREAD_THREADS_MAX 1118
#endif
#ifndef _PC_ASYNC_IO
#define _PC_ASYNC_IO 2000
#define _PC_CHOWN_RESTRICTED 2001
#define _PC_FILESIZEBITS 2002
#define _PC_LINK_MAX 2003
#define _PC_MAX_CANON 2004
#define _PC_MAX_INPUT 2005
#define _PC_NO_TRUNC 2006
#define _PC_PATH_MAX 2007
#define _PC_PIPE_BUF 2008
#define _PC_PRIO_IO 2009
#define _PC_SYMLINK_MAX 2010
#define _PC_SYNC_IO 2011
#define _PC_VDISABLE 2012
#endif
#ifndef _CS_PATH
#define _CS_PATH 3000
#define _CS_V7_ENV 3001
#endif
#ifndef _SC_ARG_MAX
#define _SC_ARG_MAX 0
size_t confstr(int name, char* buf, size_t len);
#endif
#ifndef _SC_PAGE_SIZE
#define _SC_PAGE_SIZE _SC_PAGESIZE
#endif
#ifndef _PC_NAME_MAX
#define _PC_NAME_MAX 3
#endif
long sysconf(int name);
long pathconf(const char* path, int name);

// Query current working directory (shell/vterm cwd).
char* getcwd(char* buf, size_t size);

// Change current working directory (shell/vterm cwd).
// Returns 0 on success, -1 on error.
int chdir(const char* path);
int fchdir(int fd);
int chroot(const char* path);
int setsid(void);
pid_t getsid(pid_t pid);
int setpgid(pid_t pid, pid_t pgid);
int tcsetpgrp(int fd, pid_t pgrp);
int nice(int inc);
int setgid(gid_t gid);
int setuid(uid_t uid);
int getgroups(int size, gid_t list[]);
pid_t getpid(void);
pid_t getppid(void);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int chown(const char* path, uid_t owner, gid_t group);
char* crypt(const char* key, const char* salt);
int gethostname(char* name, size_t len);
int sethostname(const char* name, size_t len);
ssize_t readlink(const char* path, char* buf, size_t bufsize);

int execv(const char* path, char* const argv[]);
int execve(const char* path, char* const argv[], char* const envp[]);
int execvp(const char* file, char* const argv[]);
int execl(const char* path, const char* arg, ...);
int execlp(const char* file, const char* arg, ...);
char* ttyname(int fd);

struct stat;
int openat(int dirfd, const char* path, int flags, ...);
int mkdirat(int dirfd, const char* path, mode_t mode);
int fstatat(int dirfd, const char* path, struct stat* st, int flags);
int faccessat(int dirfd, const char* path, int mode, int flags);
int linkat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath, int flags);
int symlinkat(const char* target, int newdirfd, const char* linkpath);
int mknodat(int dirfd, const char* path, mode_t mode, dev_t dev);
int futimens(int fd, const struct timespec times[2]);
int lchown(const char* path, uid_t owner, gid_t group);
int fchownat(int dirfd, const char* path, uid_t owner, gid_t group, int flags);
int unlinkat(int dirfd, const char* path, int flags);
ssize_t readlinkat(int dirfd, const char* path, char* buf, size_t bufsize);

/*
 * ABI-INVARIANT: SEEK_* values match POSIX and SYSCALL_LSEEK whence (110).
 */
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/* Reposition the offset of an open file descriptor. */
long lseek(int fd, long offset, int whence);
ssize_t readahead(int fd, off64_t offset, size_t count);

// Low-memory streaming file writer (EYNFS only today).
int eynfs_stream_begin(const char* path);
ssize_t eynfs_stream_write(int handle, const void* buf, size_t len);
int eynfs_stream_end(int handle);
