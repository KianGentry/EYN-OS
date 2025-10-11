#include <watchdog.h>
#include <sched.h>
#include <panic.h>
#include <string.h>

static volatile uint32 g_wd_timeout_ticks = 0; // 0 = disabled
static volatile uint32 g_wd_last_kick_ticks = 0;
static char g_wd_last_source[32] = {0};
static volatile int g_wd_armed = 0;
static volatile uint32 g_wd_tick_hz = 0; // track Hz to allow rescaling

void watchdog_init(uint32 timeout_ticks) {
    g_wd_timeout_ticks = timeout_ticks;
    g_wd_last_kick_ticks = sched_get_tick_count();
    g_wd_armed = (timeout_ticks > 0) ? 1 : 0;
    g_wd_last_source[0] = '\0';
    g_wd_tick_hz = sched_get_tick_hz();
}

void watchdog_set_timeout(uint32 timeout_ticks) {
    g_wd_timeout_ticks = timeout_ticks;
    if (timeout_ticks == 0) { g_wd_armed = 0; }
    else { g_wd_armed = 1; g_wd_last_kick_ticks = sched_get_tick_count(); }
}

void watchdog_kick(const char* source) {
    g_wd_last_kick_ticks = sched_get_tick_count();
    if (source && *source) {
        // copy up to 31 chars
        int i = 0; for (; i < 31 && source[i]; ++i) g_wd_last_source[i] = source[i];
        g_wd_last_source[i] = '\0';
    }
}

void watchdog_on_tick(void) {
    if (!g_wd_armed || g_wd_timeout_ticks == 0) return;
    // If system tick Hz changed since init, rescale timeout to preserve ~real time
    uint32 cur_hz = sched_get_tick_hz();
    if (cur_hz && g_wd_tick_hz && cur_hz != g_wd_tick_hz) {
        // new_timeout_ticks = old_timeout_ticks * new_hz / old_hz (rounded)
        uint32 nt = (g_wd_timeout_ticks / g_wd_tick_hz) * cur_hz + ((g_wd_timeout_ticks % g_wd_tick_hz) * cur_hz) / g_wd_tick_hz;
        if (nt == 0) nt = 1;
        g_wd_timeout_ticks = nt;
        g_wd_tick_hz = cur_hz;
    }
    uint32 now = sched_get_tick_count();
    uint32 elapsed = now - g_wd_last_kick_ticks; // wraps naturally mod 2^32
    if (elapsed >= g_wd_timeout_ticks) {
        // Trip the watchdog: panic with a descriptive message
        PANICF("WATCHDOG: no progress for %d ticks (last: %s)", (int)elapsed, g_wd_last_source[0] ? g_wd_last_source : "unknown");
    }
}

uint32 watchdog_get_timeout(void) { return g_wd_timeout_ticks; }
uint32 watchdog_get_ticks_since_kick(void) { return sched_get_tick_count() - g_wd_last_kick_ticks; }
const char* watchdog_get_last_source(void) { return g_wd_last_source; }
