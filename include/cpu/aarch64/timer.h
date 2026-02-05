#ifndef EYNOS_CPU_AARCH64_TIMER_H
#define EYNOS_CPU_AARCH64_TIMER_H

#include <misc/types.h>

/*
 * ARM generic timer (virtual timer) helpers for bring-up.
 */

uint32 aarch64_timer_get_freq_hz(void);
uint32 aarch64_timer_get_tick_hz(void);
void aarch64_timer_init_tick_hz(uint32 tick_hz);
void aarch64_timer_rearm(void);

#endif
