#include <misc/types.h>

/*
 * Minimal shell logging hooks for AArch64-full.
 *
 * The i386 build wires shell logging into the VGA printf implementation.
 * On AArch64-full we currently keep these as lightweight stubs so that
 * portable shell commands (e.g. `log on|off`) can be enabled without
 * pulling in the i386 VGA driver.
 *
 * When we later port the full logging pipeline, we can implement buffering
 * and persistence here (or move the shared implementation out of vga.c).
 */

int shell_log_active = 0;

void shell_log_enable(void)
{
    shell_log_active = 1;
}

void shell_log_disable(void)
{
    shell_log_active = 0;
}

void shell_log_flush(void)
{
    // no-op on AArch64 for now
}
