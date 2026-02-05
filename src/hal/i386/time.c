#include <hal/time.h>

#include <cpu/irq.h>
#include <sched.h>

static hal_time_tick_handler_t g_tick_handler;
static void* g_tick_user;

static void i386_tick_trampoline(void) {
    if (g_tick_handler) {
        g_tick_handler(g_tick_user);
    }
}

uint64 hal_time_ticks(void) {
    return (uint64)sched_get_tick_count();
}

uint32 hal_time_tick_hz(void) {
    return sched_get_tick_hz();
}

void hal_sleep_us(uint32 usec) {
    sched_sleep_us(usec);
}

int hal_time_register_tick_handler(hal_time_tick_handler_t handler, void* user) {
    g_tick_handler = handler;
    g_tick_user = user;
    /* i386 PIT tick is IRQ0. */
    register_interrupt_handler(0, i386_tick_trampoline);
    return 0;
}
