#include <cpu/aarch64/timer.h>

// Re-arm the virtual timer every tick.
void aarch64_timer_on_tick(void) {
    aarch64_timer_rearm();
}
