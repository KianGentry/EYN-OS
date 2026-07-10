#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <misc/types.h>

// Initialize watchdog with a timeout in ticks (0 disables watchdog)
void watchdog_init(uint32 timeout_ticks);

// Kick/pet the watchdog to indicate forward progress (optional source tag)
void watchdog_kick(const char* source);

// Called on each scheduler tick (IRQ0) to update the watchdog
void watchdog_on_tick(void);

// Configure timeout dynamically
void watchdog_set_timeout(uint32 timeout_ticks);

// Read current status for debugging/telemetry
uint32 watchdog_get_timeout(void);
uint32 watchdog_get_ticks_since_kick(void);
const char* watchdog_get_last_source(void);

#endif // WATCHDOG_H
