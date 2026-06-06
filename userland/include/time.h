#pragma once

#include <sys/types.h>

/*
 * ABI-INVARIANT: struct timeval layout matches POSIX / Linux on 32-bit x86.
 * tv_sec is seconds since the epoch; tv_usec is microseconds [0, 999999].
 * On EYN-OS these are derived from the PIT tick counter and have 10ms
 * resolution at the default 100Hz PIT rate.
 */
struct timeval {
    long tv_sec;
    long tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#ifndef CLOCKS_PER_SEC
#define CLOCKS_PER_SEC 1000L
#endif

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

struct sigevent;

extern long timezone;
extern int daylight;
extern char* tzname[2];

time_t time(time_t* t);
clock_t clock(void);
char* ctime(const time_t* t);
char* ctime_r(const time_t* t, char* buf);
struct tm* gmtime(const time_t* t);
struct tm* gmtime_r(const time_t* t, struct tm* out);
struct tm* localtime(const time_t* t);
struct tm* localtime_r(const time_t* t, struct tm* out);
int clock_gettime(clockid_t clk_id, struct timespec* tp);
int clock_settime(clockid_t clk_id, const struct timespec* tp);
int nanosleep(const struct timespec* req, struct timespec* rem);
int timer_create(clockid_t clockid, struct sigevent* sevp, timer_t* timerid);
int timer_settime(timer_t timerid, int flags, const struct itimerspec* new_value, struct itimerspec* old_value);
int timer_delete(timer_t timerid);
size_t strftime(char* s, size_t max, const char* format, const struct tm* tm);
time_t mktime(struct tm* tm);
char* strptime(const char* buf, const char* format, struct tm* tm);
void tzset(void);

/* Returns time in milliseconds since boot split into tv_sec + tv_usec. */
int gettimeofday(struct timeval* tv, struct timezone* tz);
int settimeofday(const struct timeval* tv, const struct timezone* tz);
