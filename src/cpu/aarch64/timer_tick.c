#include <misc/types.h>
#include <cpu/aarch64/timer.h>

// Re-arm the virtual timer every tick.
void aarch64_timer_on_tick(void) {
    uint32 freq = aarch64_timer_get_freq_hz();
    uint32 interval = freq / 100;
    if (interval == 0) interval = 1;
    asm volatile("msr cntv_tval_el0, %0" :: "r"(interval));
}
