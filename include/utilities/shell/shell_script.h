#ifndef SHELL_SCRIPT_H
#define SHELL_SCRIPT_H

#include <types.h>
#include <native_exec.h>

// shell script execution result
typedef enum {
    SHELL_SCRIPT_SUCCESS = 0,
    SHELL_SCRIPT_ERROR_FILE_NOT_FOUND,
    SHELL_SCRIPT_ERROR_INVALID_FORMAT,
    SHELL_SCRIPT_ERROR_EXECUTION_FAILED,
    SHELL_SCRIPT_ERROR_MEMORY_ALLOC
} shell_script_result_t;

// function declarations
exec_result_t execute_shell_script(const char* filename);

// Script driver helpers for the interactive shell loop.
// When a .shell script is started via execute_shell_script(), the main shell loop
// should call shell_script_next_command() to retrieve the next command to execute.
// This design allows scripts to continue after running ring3 programs (.uelf).
int shell_script_is_active(void);

// Writes the next executable command into out (NUL-terminated).
// Returns 1 if a command was produced, 0 if the script has finished.
int shell_script_next_command(char* out, uint32 outsz);

#endif // SHELL_SCRIPT_H
