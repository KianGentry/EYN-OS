# Watchdog Timer

EYN-OS includes a lightweight watchdog to detect UI or kernel stalls and trigger a diagnostic panic if the system stops making progress.

## Overview

- Driven by the scheduler tick (IRQ0)
- Times out if no component "kicks" it within the configured number of ticks
- On expiry, raises a panic with a clear message and last activity source

Typical integrations kick the watchdog from long-running loops in the shell, tiling/window manager, and other subsystems.

## API

Header: `include/misc/watchdog.h`

```c
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
```

Notes:
- The timeout is specified in "ticks." See `sched_get_tick_hz()` for the tick frequency.
- The implementation rescales the timeout if the scheduler tick rate changes at runtime.
- Passing a short source string to `watchdog_kick()` helps identify which loop last made progress.

## Integration Points

- Kernel entry configures the watchdog with a conservative default.
- `sched.c` calls `watchdog_on_tick()` each tick.
- The shell and the tiling/window manager call `watchdog_kick()` during input, mouse, and render loops.

## Failure Behavior

If the watchdog times out, the kernel calls `PANICF("WATCHDOG: no progress for %d ticks (last: %s)", ...)`, rendering a diagnostic screen (see Panic & Stop Codes) and printing a backtrace to the serial log.

## Tuning

For very slow machines or long blocking I/O, increase the timeout or add additional `watchdog_kick()` calls in lengthy operations that still represent forward progress.
