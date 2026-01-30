#include <drivers/vga.h>

// AArch64 bring-up doesn't use the VGA driver, but we reuse the existing
// shell pipeline implementation which captures command output via the
// VGA shell redirect buffer. Provide a small compatible implementation.

int shell_redirect_active = 0;
int shell_redirect_pos = 0;
char shell_redirect_buf[SHELL_REDIRECT_BUF_SIZE];

void start_shell_redirect(void) {
	shell_redirect_active = 1;
	shell_redirect_pos = 0;
	shell_redirect_buf[0] = 0;
}

void stop_shell_redirect(void) {
	shell_redirect_active = 0;
	// Keep buffer contents for consumers (pipeline/redirection).
}
