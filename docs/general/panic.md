# Panic, Assertions, and Stop Codes

EYN-OS provides a consistent diagnostic experience when critical errors occur, with a readable on-screen report and a serial backtrace for deeper debugging.

## What Happens on Panic

On a panic or failed assertion:
- Interrupts are disabled and the screen switches to a diagnostic view.
- A concise summary shows the reason, source file and line, a category, and a stable stop code (`EYNOS_XXXXXXXX`).
- A backtrace is written to the serial log (COM1) for post-mortem analysis.

See also: `docs/stop-codes.md` for interpreting stop codes and next steps.

## Developer API

Header: `include/misc/panic.h`

```c
// Unformatted panic with message, file, and line
void panic(const char* msg, const char* file, int line);

// printf-like panic formatter (supports %s, %d, %c)
void panicf(const char* file, int line, const char* fmt, ...);

// Assertion failure helper
void assert_fail(const char* expr, const char* file, int line);

// Minimal serial backtrace emitter
void backtrace(void);

// Re-entrant guard
int panic_is_in_progress(void);

// Convenience macros automatically pass file and line
#define PANIC(msg) panic((msg), __FILE__, __LINE__)
#define PANICF(fmt, ...) panicf(__FILE__, __LINE__, (fmt), __VA_ARGS__)
#define ASSERT(x) do { if (!(x)) assert_fail(#x, __FILE__, __LINE__); } while (0)
```

## Categories and Visuals

The panic renderer classifies messages into categories like ASSERT, PAGING, FILESYSTEM, and IRQ via a simple keyword scan. The screen uses a calm diagnostic theme and avoids complex UI to ensure reliability.

## Testing

Two shell commands exercise the system:
- `panic` - triggers a manual panic to verify display and serial output.
- `assertfail` - triggers an ASSERT to test the assertion path.

Use these to validate serial capture and overall diagnostic behavior in your environment.
