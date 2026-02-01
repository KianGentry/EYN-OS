#include <utilities/shell/shell_log.h>

/*
 * AArch64 bring-up logging stub.
 *
 * The minimal AArch64 target intentionally avoids pulling in the VFS stack.
 * However, the shared AArch64 printf implementation may reference the
 * shell logging API. This file satisfies those symbols without adding heavy
 * dependencies.
 *
 * The AArch64-full target links the real implementation from
 * src/utilities/shell/shell_log.c.
 */

char* shell_log_buf = 0;
int shell_log_buf_size = 0;
int shell_log_pos = 0;
int shell_log_line_count = 0;
int shell_log_line_starts[1001];
int shell_log_current_line_start = 0;
int shell_log_active = 0;

void shell_log_enable(void) { shell_log_active = 1; }
void shell_log_disable(void) { shell_log_active = 0; }

void init_dynamic_log_buffer(void) {
    // no-op for bring-up
}

void shell_log_append(const char* data, int len) {
    (void)data;
    (void)len;
    // no-op for bring-up
}

void shell_log_flush(void) {
    // no-op for bring-up
}
