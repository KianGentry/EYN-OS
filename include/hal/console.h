#ifndef EYNOS_HAL_CONSOLE_H
#define EYNOS_HAL_CONSOLE_H

#include <misc/types.h>

/*
 * HAL Console
 *
 * Purpose:
 *  Provide a single, portable "text stream" output for early boot, the shell,
 *  and kernel logging.
 *
 * Contract:
 *  - Must be callable before the heap is initialized.
 *  - Must be safe to call from IRQ context unless a backend documents otherwise.
 *  - Control characters must behave consistently across architectures:
 *      '\n' newline, '\r' carriage return, '\b' and 0x7F erase/backspace.
 */

/* Write a single byte to the active console. */
void hal_console_putc(char c);

/* Write a NUL-terminated string to the active console. */
void hal_console_write(const char* s);

/* Write a bounded buffer to the active console. */
void hal_console_write_len(const char* s, uint32 len);

/* Best-effort set of the default text color (RGB). Safe no-op if unsupported. */
void hal_console_set_rgb(uint8 r, uint8 g, uint8 b);

#endif
