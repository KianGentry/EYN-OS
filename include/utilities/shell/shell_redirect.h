#ifndef EYNOS_UTILITIES_SHELL_REDIRECT_H
#define EYNOS_UTILITIES_SHELL_REDIRECT_H

/*
 * Shell output capture / redirection API.
 *
 * This is intentionally NOT tied to VGA: it is used by the shell pipeline and
 * by printf implementations to capture command output into a bounded buffer.
 */

// Shell redirection buffer size (lowered for small-memory systems)
#define SHELL_REDIRECT_BUF_SIZE 1024

// 1 when capture is active
extern int shell_redirect_active;

// Current write position in shell_redirect_buf while redirect is active.
extern int shell_redirect_pos;

// Captured output buffer (NUL-terminated).
extern char shell_redirect_buf[SHELL_REDIRECT_BUF_SIZE];

// Begin/stop capturing stdout into shell_redirect_buf.
void start_shell_redirect(void);
void stop_shell_redirect(void);

#endif
