#ifndef SHELL_SCRIPT_H
#define SHELL_SCRIPT_H

#include <misc/types.h>

/*
 * EYN-OS Shell Script Interpreter
 *
 * Executes .shell script files from the EYNFS filesystem.  Scripts are
 * line-oriented text files that support:
 *
 *   - Comments:            # this is a comment
 *   - Variables:           set NAME=value   /   $NAME or ${NAME}
 *   - Script arguments:    $0 (script name), $1, $2, ... $9
 *   - Command substitution: $(command args)
 *   - Conditionals:        if COND / elif COND / else / endif
 *   - While loops:         while COND / endwhile
 *   - Exit:                exit [code]
 *
 * Condition syntax (for if / elif / while):
 *   STR1 == STR2           string equality
 *   STR1 != STR2           string inequality
 *   -e PATH                file/dir exists
 *   -f PATH                file exists (not dir)
 *   WORD                   true if non-empty and not "0"
 *
 * Commands are dispatched through the same path as interactive shell input
 * (handle_shell_command), so all /binaries programs, aliases, and pipelines
 * work transparently.
 */

/*
 * Run a .shell script from the filesystem.
 *
 * drive    -- physical drive number (e.g. g_current_drive)
 * path     -- absolute path to the script file on EYNFS
 * argc     -- number of extra arguments passed to the script
 * argv     -- argument values ($1..$N); may be NULL if argc == 0
 *
 * Returns 0 on success, negative on error (file not found, parse error, etc.).
 */
int shell_script_run(uint8 drive, const char *path, int argc, const char *const *argv);

#endif /* SHELL_SCRIPT_H */
