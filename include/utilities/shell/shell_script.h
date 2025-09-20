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

#endif // SHELL_SCRIPT_H
