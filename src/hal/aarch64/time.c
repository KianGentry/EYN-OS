#include <hal/time.h>

#include <arch.h>
#include <cpu/aarch64/irq_router.h>
#include <cpu/aarch64/timer.h>

/* Tick counter is maintained by the AArch64 IRQ dispatch path. */
extern volatile uint32 g_aarch64_ticks;

uint64 hal_time_ticks(void) {
    return (uint64)g_aarch64_ticks;
}

uint32 hal_time_tick_hz(void) {
    return aarch64_timer_get_tick_hz();
}

void hal_sleep_us(uint32 usec) {
    uint32 hz = hal_time_tick_hz();
    if (hz == 0) hz = 100;

    uint64 start = hal_time_ticks();
    uint64 needed_ticks = ((uint64)usec * (uint64)hz + 999999ull) / 1000000ull;
    if (needed_ticks == 0) needed_ticks = 1;
    uint64 target = start + needed_ticks;

    while (hal_time_ticks() < target) {
        arch_halt();
    }
}

int hal_time_register_tick_handler(hal_time_tick_handler_t handler, void* user) {
    aarch64_irq_set_tick_handler((aarch64_tick_handler_t)handler, user);
    return 0;
}
