#ifndef EYNOS_HAL_TIME_H
#define EYNOS_HAL_TIME_H

#include <misc/types.h>

/*
 * HAL Time
 *
 * Purpose:
 *  Provide a stable tick/timebase API for scheduling, sleep, and profiling.
 *
 * Contract:
 *  - hal_time_ticks() is monotonic and increments at hal_time_tick_hz().
 *  - hal_sleep_us() is cooperative (may yield to UI/scheduler work).
 */

/* Monotonic tick counter. */
uint64 hal_time_ticks(void);

/* Tick frequency in Hz. Must be non-zero once the timer is initialized. */
uint32 hal_time_tick_hz(void);

/* Best-effort cooperative sleep. */
void hal_sleep_us(uint32 usec);

#endif
