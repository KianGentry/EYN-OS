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

time_t time(time_t* t);
char* ctime_r(const time_t* t, char* buf);
struct tm* localtime(const time_t* t);
struct tm* localtime_r(const time_t* t, struct tm* out);

/* Returns time in milliseconds since boot split into tv_sec + tv_usec. */
int gettimeofday(struct timeval* tv, struct timezone* tz);
