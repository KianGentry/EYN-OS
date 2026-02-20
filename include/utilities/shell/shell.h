#ifndef SHELL_H
#define SHELL_H
#include <types.h>

struct shell_args;
typedef struct shell_args shell_args_t;

#define MAX_HISTORY_SIZE 50
#define MAX_COMMAND_LENGTH 200

// Command history structure
typedef struct {
    char commands[MAX_HISTORY_SIZE][MAX_COMMAND_LENGTH];
    int count;
    int current;
} command_history_t;

// Function declarations
void launch_shell(int n);
void handle_shell_command(string input);
int get_command_execution_errors();
string get_last_failed_command();
string readStr_with_history(command_history_t* history);
void add_to_history(command_history_t* history, const char* command);
void clear_history(command_history_t* history);
void show_history(command_history_t* history);

// Command lookup function
typedef void (*shell_cmd_handler_t)(const shell_args_t* args);
shell_cmd_handler_t find_command(const char* name);

// Drive context helpers used by shell GUIs and filesystem code
uint8 get_current_physical_drive(void);
uint8 get_current_logical_drive(void);

// Global history instance
extern command_history_t g_command_history;

#endif
