#ifndef EYNOS_UTILITIES_SHELL_LOG_H
#define EYNOS_UTILITIES_SHELL_LOG_H

#include <misc/types.h>

/*
 * Shell logging API.
 *
 * This intentionally avoids VGA/EYNFS dependencies so that logging works on
 * both i386 and AArch64. Log persistence is implemented via the VFS facade.
 *
 * Behavior:
 * - When enabled, callers should feed formatted output to shell_log_append().
 * - The implementation buffers output and flushes on newline.
 * - Flush appends to a bounded log file at "/log".
 */

// Max in-memory buffer size (upper bound; actual allocation may be smaller).
#define LOG_BUF_SIZE 65536

// Global logging state (kept for compatibility with existing shell code).
extern int shell_log_active;
extern char* shell_log_buf;
extern int shell_log_buf_size;
extern int shell_log_pos;

// Track start positions of last 1000 lines in the in-memory log buffer.
extern int shell_log_line_count;
extern int shell_log_line_starts[1001];
extern int shell_log_current_line_start;

void shell_log_enable(void);
void shell_log_disable(void);

// Ensure log buffer exists (safe no-op if already allocated).
void init_dynamic_log_buffer(void);

// Append bytes to the log buffer (flushes on newline).
void shell_log_append(const char* data, int len);

// Force flush buffered log bytes to "/log" (bounded append).
void shell_log_flush(void);

#endif
