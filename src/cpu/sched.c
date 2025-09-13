#include <sched.h>
#include <system.h>
#include <irq.h>

static volatile uint32 g_ticks = 0;
static uint32 g_tick_hz = 100;
static uint32 g_timeslice_ticks = 5; // ~50ms at 100Hz
static uint32 g_current_slice = 0;

static void sched_irq0_handler(void) {
    sched_tick();
}

void sched_init(void) {
    g_ticks = 0;
    // register tick handler on IRQ0
    register_interrupt_handler(0, sched_irq0_handler);
}

void sched_yield(void) {
    // cooperative yield: no-op for now
    g_current_slice = 0; // force timeslice end handling on next tick
}

void sched_sleep_us(uint32 microseconds) {
    if (g_tick_hz == 0) {
        extern void eyn_kernel_delay(uint32 microseconds);
        eyn_kernel_delay(microseconds);
        return;
    }
    // convert requested microseconds to ticks using integer ceil
    uint32 tick_us = 1000000U / g_tick_hz;
    if (tick_us == 0) tick_us = 1;
    uint32 needed_ticks = (microseconds + tick_us - 1) / tick_us;
    uint32 target_ticks = g_ticks + needed_ticks;
    while ((uint32)g_ticks < target_ticks) {
        // halt until next interrupt to save cpu
        __asm__ __volatile__("hlt");
    }
}

void sched_tick(void) {
    g_ticks++;
    if (++g_current_slice >= g_timeslice_ticks) {
        g_current_slice = 0;
        extern void sched_on_timeslice_end(void);
        sched_on_timeslice_end();
    }
}

void sched_set_tick_hz(uint32 hz) {
    g_tick_hz = (hz == 0) ? 100 : hz;
}

void sched_set_timeslice_ticks(uint32 ticks) {
    if (ticks == 0) ticks = 1;
    g_timeslice_ticks = ticks;
    if (g_current_slice >= g_timeslice_ticks) g_current_slice = 0;
}

// default weak hook: does nothing; will be implemented with real context switches later
__attribute__((weak)) void sched_on_timeslice_end(void) {}


