#ifndef SHELL_COMMAND_INFO_H
#define SHELL_COMMAND_INFO_H

#include <types.h>

// Command types
typedef enum {
    CMD_ESSENTIAL = 0,    // Always in RAM
    CMD_STREAMING = 1,    // Loaded on demand
    CMD_DISABLED = 2      // Not available
} command_type_t;

// Command handler function type
typedef void (*shell_cmd_handler_t)(string arg);

// Enhanced command information structure
typedef struct {
    const char* name;
    shell_cmd_handler_t handler;
    command_type_t type;
    const char* description;
    const char* example;
} shell_command_info_t;

// Enhanced macro to register a shell command in the .shellcmds linker section
#define REGISTER_SHELL_COMMAND(var, cmd_name, handler_func, cmd_type, desc, ex) \
    __attribute__((section(".shellcmds"), used)) \
    const shell_command_info_t shell_cmd_info_##var = { cmd_name, handler_func, cmd_type, desc, ex }

// Externs for start/stop of the section (set by linker script)
extern const shell_command_info_t __start_shellcmds[];
extern const shell_command_info_t __stop_shellcmds[];

#endif // SHELL_COMMAND_INFO_H 