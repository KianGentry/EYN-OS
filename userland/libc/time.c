#include <time.h>
#include <string.h>
#include <eynos_syscall.h>

time_t time(time_t* t) {
    if (t) *t = 0;
    return 0;
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

char* ctime_r(const time_t* t, char* buf) {
    (void)t;
    // 26 bytes including trailing NUL is typical, but we only need a stable string.
    const char* s = "Thu Jan  1 00:00:00 1970\n";
    if (!buf) return NULL;
    strncpy(buf, s, 26);
    buf[25] = '\0';
    return buf;
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
