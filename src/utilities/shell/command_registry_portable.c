#include <shell_command_info.h>
#include <misc/types.h>

// Portable command registry:
// This TU exists so AArch64-full can select a stable subset of commands without
// pulling in i386-only subsystems (tiler/REI, PCI/e1000, paging/VMM, etc.).
//
// The actual implementations live in their normal modules (shell.c wrappers,
// shell_commands.c, fs_commands.c, gfx_cmd.c, ...). We just register them.

// Shared shell wrappers (defined in src/utilities/shell/shell.c)
void help_cmd(string arg);
void echo_cmd(string arg);
void ver_cmd(string arg);
void calc_cmd(string arg);
void random_cmd(string arg);
void sort_cmd(string arg);
void clear_cmd(string arg);
void handler_exit(string arg);

// FS wrappers (some are also in shell.c; keep these as the wrapper names used
// by the existing build)
void ls_cmd(string arg);
void read_cmd(string arg);
void cd(string arg);
void del(string arg);

// AArch64-full VFS-backed cat implementation lives in fs_commands.c
void cat_cmd(string arg);

// gfxdemo implementation lives in gfx_cmd.c
void cmd_gfxdemo(string arg);

// Additional portable commands implemented in shell_commands.c
void history_cmd(string arg);
void search_cmd(string arg);
void hexdump_cmd(string arg);
void log_cmd(string arg);

REGISTER_SHELL_COMMAND(help, "help", help_cmd, CMD_ESSENTIAL,
                       "List available commands.", "help");
REGISTER_SHELL_COMMAND(echo, "echo", echo_cmd, CMD_STREAMING,
                       "Print arguments.", "echo hello");
REGISTER_SHELL_COMMAND(ver, "ver", ver_cmd, CMD_STREAMING,
                       "Show version information.", "ver");
REGISTER_SHELL_COMMAND(calc, "calc", calc_cmd, CMD_STREAMING,
                       "32-bit fixed-point calculator.", "calc 2.5+3.7");
REGISTER_SHELL_COMMAND(random, "random", random_cmd, CMD_STREAMING,
                       "Generate random numbers.", "random 5");
REGISTER_SHELL_COMMAND(sort, "sort", sort_cmd, CMD_STREAMING,
                       "Sort strings alphabetically.", "sort zebra apple");
REGISTER_SHELL_COMMAND(clear, "clear", clear_cmd, CMD_ESSENTIAL,
                       "Clear the screen.", "clear");
REGISTER_SHELL_COMMAND(exit, "exit", handler_exit, CMD_ESSENTIAL,
                       "Exit the shell.", "exit");

REGISTER_SHELL_COMMAND(history, "history", history_cmd, CMD_STREAMING,
                       "Show command history.", "history");
REGISTER_SHELL_COMMAND(search, "search", search_cmd, CMD_STREAMING,
                       "Search for text in filenames and file contents.", "search hello -a");
REGISTER_SHELL_COMMAND(hexdump, "hexdump", hexdump_cmd, CMD_STREAMING,
                       "Print a hex dump of a file.", "hexdump test.eyn");

REGISTER_SHELL_COMMAND(log, "log", log_cmd, CMD_STREAMING,
                       "Enable or disable shell logging (AArch64: stubbed).", "log on");

// Filesystem (VFS)
REGISTER_SHELL_COMMAND(ls, "ls", ls_cmd, CMD_STREAMING,
                       "List a directory.", "ls /");
REGISTER_SHELL_COMMAND(cd, "cd", cd, CMD_STREAMING,
                       "Change directory.", "cd /testdir");
REGISTER_SHELL_COMMAND(read, "read", read_cmd, CMD_STREAMING,
                       "Display a text file (raw on AArch64).", "read test.txt");
REGISTER_SHELL_COMMAND(cat, "cat", cat_cmd, CMD_STREAMING,
                       "Print a text file.", "cat test.txt");
REGISTER_SHELL_COMMAND(del, "del", del, CMD_STREAMING,
                       "Delete a file.", "del test.txt");

// Diagnostics
REGISTER_SHELL_COMMAND(gfxdemo, "gfxdemo", cmd_gfxdemo, CMD_DIAGNOSTIC,
                       "Draw a simple gfx test pattern.", "gfxdemo");
