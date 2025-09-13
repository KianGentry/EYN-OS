#ifndef SCHED_H
#define SCHED_H

#include <types.h>

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

#endif // SCHED_H


