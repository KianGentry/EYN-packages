#ifndef SIGNAL_H
#define SIGNAL_H

#include <sys/types.h>

/* Minimal signal numbers for UELF programs */
#define SIGHUP  1
#define SIGINT  2
#define SIGQUIT 3
#define SIGILL  4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS  7
#define SIGFPE  8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTKFLT 16
/* Child status changed (POSIX) */
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGIO 29
#define SIGPOLL SIGIO
#define SIGPWR 30
#define SIGSYS 31

#define NSIG 32

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

typedef void (*sighandler_t)(int);

typedef union sigval {
    int sival_int;
    void* sival_ptr;
} sigval_t;

#define SIGEV_SIGNAL 0
#define SIGEV_NONE   1
#define SIGEV_THREAD 2

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* Basic sigset_t for mask operations */
typedef unsigned long sigset_t;

typedef struct siginfo {
    int si_signo;
    int si_errno;
    int si_code;
    pid_t si_pid;
    uid_t si_uid;
    int si_status;
    void* si_addr;
} siginfo_t;

#ifndef SA_NOCLDSTOP
#define SA_NOCLDSTOP 0x00000001
#endif
#ifndef SA_SIGINFO
#define SA_SIGINFO   0x00000004
#endif
#ifndef SA_RESTART
#define SA_RESTART   0x10000000
#endif

#ifndef CLD_EXITED
#define CLD_EXITED    1
#define CLD_KILLED    2
#define CLD_DUMPED    3
#define CLD_TRAPPED   4
#define CLD_STOPPED   5
#define CLD_CONTINUED 6
#endif

/* Minimal sigaction structure */
struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t*, void*);
    } __sigaction_handler;
    sigset_t sa_mask;
    int sa_flags;
};

#define sa_handler __sigaction_handler.sa_handler
#define sa_sigaction __sigaction_handler.sa_sigaction

struct sigevent {
    int sigev_notify;
    int sigev_signo;
    sigval_t sigev_value;
    void (*sigev_notify_function)(sigval_t);
    void* sigev_notify_attributes;
};

/* Install a simple handler (BSD-style) - provided by libc. */
sighandler_t signal(int sig, sighandler_t handler);

/* POSIX-style sigaction: minimal implementation mapping to kernel signal syscall. */
int sigaction(int signum, const struct sigaction* act, struct sigaction* oldact);

/* sigset operations */
int sigemptyset(sigset_t* set);
int sigaddset(sigset_t* set, int signo);
int sigfillset(sigset_t* set);
int sigprocmask(int how, const sigset_t* set, sigset_t* oldset);

#endif
