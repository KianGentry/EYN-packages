#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <eynos_syscall.h>

long timezone = 0;
int daylight = 0;
char* tzname[2] = {"UTC", "UTC"};

time_t time(time_t* t) {
    unsigned int ms = (unsigned int)eyn_syscall0(EYN_SYSCALL_GET_TICKS_MS);
    time_t now = (time_t)(ms / 1000u);
    if (t) *t = now;
    return now;
}

clock_t clock(void) {
    unsigned int ms = (unsigned int)eyn_syscall0(EYN_SYSCALL_GET_TICKS_MS);
    return (clock_t)ms;
}

/*
 * gettimeofday() -- return wall-clock time derived from the PIT tick counter.
 *
 * EYN-OS has no real-time clock, so tv_sec counts seconds since kernel boot
 * (not the Unix epoch).  Resolution is 1000/hz ms (10ms at default 100Hz).
 * This is sufficient for DOOM's I_GetTime() which only needs relative timing.
 *
 * tz is ignored (no timezone support).
 */
int gettimeofday(struct timeval* tv, struct timezone* tz) {
    (void)tz;
    if (!tv) return -1;
    unsigned int ms = (unsigned int)eyn_syscall0(EYN_SYSCALL_GET_TICKS_MS);
    tv->tv_sec  = (long)(ms / 1000u);
    tv->tv_usec = (long)((ms % 1000u) * 1000u);
    return 0;
}

int settimeofday(const struct timeval* tv, const struct timezone* tz) {
    (void)tv;
    (void)tz;
    errno = ENOSYS;
    return -1;
}

int clock_gettime(clockid_t clk_id, struct timespec* tp) {
    if (!tp) {
        errno = EINVAL;
        return -1;
    }
    if (clk_id != CLOCK_REALTIME && clk_id != CLOCK_MONOTONIC) {
        errno = EINVAL;
        return -1;
    }

    unsigned int ms = (unsigned int)eyn_syscall0(EYN_SYSCALL_GET_TICKS_MS);
    tp->tv_sec = (time_t)(ms / 1000u);
    tp->tv_nsec = (long)((ms % 1000u) * 1000000u);
    return 0;
}

int clock_settime(clockid_t clk_id, const struct timespec* tp) {
    (void)clk_id;
    (void)tp;
    errno = ENOSYS;
    return -1;
}

int nanosleep(const struct timespec* req, struct timespec* rem) {
    if (!req) {
        errno = EINVAL;
        return -1;
    }
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }

    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }

    if (req->tv_sec > 0) {
        unsigned int whole = (unsigned int)req->tv_sec;
        while (whole--) {
            (void)usleep(1000000u);
        }
    }
    if (req->tv_nsec > 0) {
        unsigned int usec = (unsigned int)(req->tv_nsec / 1000L);
        if (usec > 0) {
            (void)usleep(usec);
        }
    }
    return 0;
}

int timer_create(clockid_t clockid, struct sigevent* sevp, timer_t* timerid) {
    (void)clockid;
    (void)sevp;
    (void)timerid;
    errno = ENOSYS;
    return -1;
}

int timer_settime(timer_t timerid, int flags, const struct itimerspec* new_value, struct itimerspec* old_value) {
    (void)timerid;
    (void)flags;
    (void)new_value;
    (void)old_value;
    errno = ENOSYS;
    return -1;
}

int timer_delete(timer_t timerid) {
    (void)timerid;
    errno = ENOSYS;
    return -1;
}

size_t strftime(char* s, size_t max, const char* format, const struct tm* tm) {
    if (!s || !format || !tm || max == 0) return 0;

    int n = 0;
    for (const char* p = format; *p && (size_t)n + 1 < max; p++) {
        if (*p != '%') {
            s[n++] = *p;
            continue;
        }
        p++;
        if (!*p) break;

        if (*p == 'F') {
            int y = tm->tm_year + 1900;
            int m = tm->tm_mon + 1;
            int d = tm->tm_mday;
            int w = snprintf(s + n, max - (size_t)n, "%04d-%02d-%02d", y, m, d);
            if (w < 0 || (size_t)w >= max - (size_t)n) break;
            n += w;
        } else if (*p == 'T') {
            int w = snprintf(s + n, max - (size_t)n, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
            if (w < 0 || (size_t)w >= max - (size_t)n) break;
            n += w;
        } else {
            s[n++] = '%';
            if ((size_t)n + 1 >= max) break;
            s[n++] = *p;
        }
    }

    s[n] = '\0';
    return (size_t)n;
}

void tzset(void) {
    /* EYN-OS currently has no timezone database. */
}

time_t mktime(struct tm* tm) {
    if (!tm) return (time_t)-1;

    int year = tm->tm_year + 1900;
    int month = tm->tm_mon;
    int mday = tm->tm_mday;
    int hour = tm->tm_hour;
    int min = tm->tm_min;
    int sec = tm->tm_sec;

    static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};

    long days = 0;
    for (int y = 1970; y < year; y++) {
        days += 365;
        if ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) days++;
    }
    for (int m = 0; m < month; m++) {
        days += mdays[m];
        if (m == 1 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) days++;
    }
    days += (mday - 1);

    return (time_t)(days * 86400L + hour * 3600L + min * 60L + sec);
}

char* strptime(const char* buf, const char* format, struct tm* tm) {
    if (!buf || !format || !tm) return NULL;

    if (strcmp(format, "%Y-%m-%d") == 0) {
        int y, m, d;
        if (sscanf(buf, "%d-%d-%d", &y, &m, &d) == 3) {
            tm->tm_year = y - 1900;
            tm->tm_mon = m - 1;
            tm->tm_mday = d;
            while (*buf && *buf != '\n') buf++;
            return (char*)buf;
        }
    }
    return NULL;
}

char* ctime_r(const time_t* t, char* buf) {
    (void)t;
    // 26 bytes including trailing NUL is typical, but we only need a stable string.
    const char* s = "Thu Jan  1 00:00:00 1970\n";
    if (!buf) return NULL;
    strncpy(buf, s, 26);
    buf[25] = '\0';
    return buf;
}

    char* ctime(const time_t* t) {
        static char buf[32];
        return ctime_r(t, buf);
    }

struct tm* localtime_r(const time_t* t, struct tm* out) {
    (void)t;
    if (!out) return NULL;
    memset(out, 0, sizeof(*out));
    out->tm_mday = 1;
    out->tm_mon = 0;
    out->tm_year = 70;
    return out;
}

struct tm* localtime(const time_t* t) {
    static struct tm tm;
    return localtime_r(t, &tm);
}

    struct tm* gmtime_r(const time_t* t, struct tm* out) {
        return localtime_r(t, out);
    }

    struct tm* gmtime(const time_t* t) {
        static struct tm out;
        return gmtime_r(t, &out);
    }
