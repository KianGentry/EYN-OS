#ifndef SCHED_H
#define SCHED_H

#include <misc/types.h>

// simple cooperative scheduler stubs

// initialize scheduler state
void sched_init(void);

// yield the cpu cooperatively
void sched_yield(void);

// sleep for approximately microseconds (busy-wait fallback)
void sched_sleep_us(uint32 microseconds);

// tick hook for future timer integration
void sched_tick(void);

// inform scheduler of tick frequency (hz)
void sched_set_tick_hz(uint32 hz);

// optional: set timeslice length in ticks for preemption hinting
void sched_set_timeslice_ticks(uint32 ticks);

// called when a timeslice ends to trigger a context switch (stub)
void sched_on_timeslice_end(void);

// Lightweight timing/usage getters ---
// Monotonic scheduler tick counter (increments in IRQ0 handler)
uint32 sched_get_tick_count(void);
// Configured tick frequency in Hz
uint32 sched_get_tick_hz(void);
// Count of idle halts performed inside sched_sleep_us (used to estimate idle time)
uint32 sched_get_idle_hlt_count(void);

#endif // SCHED_H


