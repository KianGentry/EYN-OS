#ifndef PIPELINE_H
#define PIPELINE_H

#include <misc/types.h>

// Pipeline and redirection system for EYN-OS

// Command types in a pipeline
typedef enum {
    PIPELINE_CMD_SIMPLE,     // Simple command: ls
    PIPELINE_CMD_PIPE,       // Pipe: ls | grep
    PIPELINE_CMD_REDIRECT_IN,  // Input redirection: cmd < file
    PIPELINE_CMD_REDIRECT_OUT, // Output redirection: cmd > file
    PIPELINE_CMD_REDIRECT_APPEND, // Append redirection: cmd >> file
    PIPELINE_CMD_BACKGROUND  // Background execution: cmd &
} pipeline_command_type_t;

// Redirection types
typedef enum {
    REDIR_NONE,
    REDIR_INPUT,    // <
    REDIR_OUTPUT,   // >
    REDIR_APPEND,   // >>
    REDIR_PIPE_IN,  // Pipe input
    REDIR_PIPE_OUT  // Pipe output
} redirection_type_t;

// File descriptor structure
typedef struct {
    int fd;                    // File descriptor number
    redirection_type_t type;   // Type of redirection
    char* filename;            // Filename for file redirection
    int pipe_fd;              // Pipe file descriptor
    int is_open;              // Is this descriptor open?
} file_descriptor_t;

// Command structure
typedef struct command {
    char* name;                // Command name
    char** args;               // Command arguments
    int argc;                  // Number of arguments
    pipeline_command_type_t type;       // Command type (updated)
    file_descriptor_t* fds;    // File descriptors
    int fd_count;              // Number of file descriptors
    struct command* next;      // Next command in pipeline
    struct command* prev;      // Previous command in pipeline
    int background;            // Run in background
    int pid;                   // Process ID when running
} command_t;

// Pipeline structure
typedef struct {
    command_t* first;          // First command in pipeline
    command_t* last;           // Last command in pipeline
    int command_count;         // Number of commands
    int background;            // Run entire pipeline in background
} pipeline_t;

// Background process structure
typedef struct {
    int pid;                   // Process ID
    char* command;             // Command string
    int status;                // Exit status
    int active;                // Is process active?
} background_process_t;

// Constants
#define MAX_FDS 256
#define MAX_BACKGROUND_PROCESSES 16
#define STDIN_FD 0
#define STDOUT_FD 1
#define STDERR_FD 2

// Global variable for pipeline input data
extern char* g_pipeline_input_data;

// Function prototypes
void init_pipeline_system(void);
void cleanup_pipeline_system(void);
int is_pipeline_command(const char* input);
int count_pipeline_commands(const char* input);
char* trim_whitespace(char* str);
int is_whitespace(char c);
char* simple_strtok(char* str, const char* delims);
char** split_command(const char* cmd_str, int* argc);
void free_args(char** args, int argc);
char* parse_redirections(const char* cmd_str, command_t* cmd);
char* read_file_for_input_redirection(const char* filename);
command_t* parse_command(const char* cmd_str);
pipeline_t* parse_pipeline(const char* input);
void free_command(command_t* cmd);
void free_pipeline(pipeline_t* pipeline);
int allocate_file_descriptor(void);
file_descriptor_t* get_file_descriptor(int fd);
void close_fd(int fd);
int execute_simple_command(command_t* cmd);
int execute_background_command(command_t* cmd);
int execute_command(command_t* cmd);
int execute_pipeline(pipeline_t* pipeline);
int execute_complex_pipeline(pipeline_t* pipeline);

// Background process management
int add_background_process(int pid, const char* command);
int remove_background_process(int pid);
background_process_t* get_background_process(int pid);
void list_background_processes(void);
void wait_for_background_process(int pid);
void wait_for_all_background_processes(void);

#endif
