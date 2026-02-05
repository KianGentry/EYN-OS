#include <cpu/aarch64/timer.h>

static uint32 g_cntv_interval = 0;
static uint32 g_tick_hz = 0;

uint32 aarch64_timer_get_freq_hz(void) {
    uint32 v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

void aarch64_timer_init_tick_hz(uint32 tick_hz) {
    if (tick_hz == 0) tick_hz = 100;

    g_tick_hz = tick_hz;

    uint32 freq = aarch64_timer_get_freq_hz();
    uint32 interval = freq / tick_hz;
    if (interval == 0) interval = 1;

    g_cntv_interval = interval;

    // Program virtual timer compare value and enable it.
    asm volatile("msr cntv_tval_el0, %0" :: "r"(interval));

    // CNTV_CTL_EL0: bit0 ENABLE=1, bit1 IMASK=0, bit2 ISTATUS (RO)
    uint32 ctl = 1;
    asm volatile("msr cntv_ctl_el0, %0" :: "r"(ctl));
}

uint32 aarch64_timer_get_tick_hz(void) {
    if (g_tick_hz != 0) return g_tick_hz;
    return 100;
}

void aarch64_timer_rearm(void) {
    uint32 interval = g_cntv_interval;
    if (interval == 0) {
        /* Default to 100Hz if init wasn't called yet. */
        uint32 freq = aarch64_timer_get_freq_hz();
        interval = freq / 100;
        if (interval == 0) interval = 1;
        g_cntv_interval = interval;
    }
    asm volatile("msr cntv_tval_el0, %0" :: "r"(interval));
}
