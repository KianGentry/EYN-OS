#pragma once

#include <sys/types.h>

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
