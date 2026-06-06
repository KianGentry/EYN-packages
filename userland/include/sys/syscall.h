#pragma once

/* Minimal Linux-style syscall numbers used by toybox portability wrappers. */
#ifndef __NR_copy_file_range
#define __NR_copy_file_range 377
#endif

#ifndef __NR_ioprio_set
#define __NR_ioprio_set 289
#endif
#ifndef __NR_ioprio_get
#define __NR_ioprio_get 290
#endif

#ifndef __NR_pivot_root
#define __NR_pivot_root 155
#endif
#ifndef SYS_pivot_root
#define SYS_pivot_root __NR_pivot_root
#endif

#ifndef __NR_delete_module
#define __NR_delete_module 129
#endif

#ifndef __NR_sched_setaffinity
#define __NR_sched_setaffinity 241
#endif
#ifndef __NR_sched_getaffinity
#define __NR_sched_getaffinity 242
#endif

#ifndef __NR_sched_setattr
#define __NR_sched_setattr 351
#endif
#ifndef __NR_sched_getattr
#define __NR_sched_getattr 352
#endif

#ifndef SYS_timer_create
#define SYS_timer_create 259
#endif
#ifndef SYS_timer_settime
#define SYS_timer_settime 260
#endif

#ifndef SYS_renameat2
#define SYS_renameat2 353
#endif

#ifndef SYS_sched_setscheduler
#define SYS_sched_setscheduler 156
#endif
#ifndef SYS_sched_getscheduler
#define SYS_sched_getscheduler 157
#endif
#ifndef SYS_sched_getparam
#define SYS_sched_getparam 155
#endif
#ifndef SYS_sched_get_priority_max
#define SYS_sched_get_priority_max 159
#endif
#ifndef SYS_sched_get_priority_min
#define SYS_sched_get_priority_min 160
#endif

#ifndef SYS_init_module
#define SYS_init_module 128
#endif
#ifndef SYS_finit_module
#define SYS_finit_module 350
#endif

#ifndef SYS_setns
#define SYS_setns 346
#endif
#ifndef SYS_unshare
#define SYS_unshare 310
#endif

long syscall(long number, ...);
