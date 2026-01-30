#include <pipeline.h>
#include <shell.h>
#include <shell_commands.h>
#include <shell_command_info.h>
#include <fs_commands.h>
#include <util.h>
#include <vga.h>
#include <string.h>
#include <eynfs.h>
#include <misc/types.h>
#include <utilities/shell/alias.h>

// Global variables
file_descriptor_t g_file_descriptors[MAX_FDS];
background_process_t g_background_processes[MAX_BACKGROUND_PROCESSES];
int g_next_fd = 3;  // Start after stdin, stdout, stderr
int g_next_bg_pid = 1;
char* g_pipeline_input_data = NULL;  // Pipeline input data for filter commands

// Initialize pipeline system
void init_pipeline_system(void) {
    // Initialize file descriptors
    for (int i = 0; i < MAX_FDS; i++) {
        g_file_descriptors[i].fd = i;
        g_file_descriptors[i].type = REDIR_NONE;
        g_file_descriptors[i].filename = NULL;
        g_file_descriptors[i].pipe_fd = -1;
        g_file_descriptors[i].is_open = (i < 3); // stdin, stdout, stderr are open
    }
    
    // Initialize background processes
    for (int i = 0; i < MAX_BACKGROUND_PROCESSES; i++) {
        g_background_processes[i].pid = 0;
        g_background_processes[i].command = NULL;
        g_background_processes[i].status = 0;
        g_background_processes[i].active = 0;
    }
}

// Cleanup pipeline system
void cleanup_pipeline_system(void) {
    // Close all open file descriptors
    for (int i = 0; i < MAX_FDS; i++) {
        if (g_file_descriptors[i].is_open && i >= 3) {
            close_fd(i);
        }
    }
    
    // Clean up background processes
    for (int i = 0; i < MAX_BACKGROUND_PROCESSES; i++) {
        if (g_background_processes[i].active) {
            if (g_background_processes[i].command) {
                free(g_background_processes[i].command);
                g_background_processes[i].command = NULL;
            }
            g_background_processes[i].active = 0;
        }
    }
}

// Check if input contains pipeline operators
int is_pipeline_command(const char* input) {
    if (!input) return 0;
    
    for (int i = 0; input[i]; i++) {
        if (input[i] == '|' || input[i] == '<' || input[i] == '>' || input[i] == '&') {
            return 1;
        }
    }
    return 0;
}

// Count commands in pipeline
int count_pipeline_commands(const char* input) {
    if (!input) return 0;
    
    int count = 1;
    for (int i = 0; input[i]; i++) {
        if (input[i] == '|') {
            count++;
        }
    }
    return count;
}

// Trim whitespace from string
char* trim_whitespace(char* str) {
    if (!str) return NULL;
    
    // Trim leading whitespace
    while (*str && is_whitespace(*str)) str++;
    
    // Trim trailing whitespace
    char* end = str + strlen(str) - 1;
    while (end > str && is_whitespace(*end)) {
        *end = '\0';
        end--;
    }
    
    return str;
}

// Check if character is whitespace
int is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Simple string tokenizer (replaces strtok)
char* simple_strtok(char* str, const char* delims) {
    static char* last_str = NULL;
    static char* current_pos = NULL;
    
    if (str) {
        last_str = str;
        current_pos = str;
    } else if (!last_str) {
        return NULL;
    }
    
    // Skip leading delimiters
    while (*current_pos && strchr(delims, *current_pos)) {
        current_pos++;
    }
    
    if (!*current_pos) {
        last_str = NULL;
        return NULL;
    }
    
    char* token_start = current_pos;
    
    // Find end of token
    while (*current_pos && !strchr(delims, *current_pos)) {
        current_pos++;
    }
    
    if (*current_pos) {
        *current_pos = '\0';
        current_pos++;
    }
    
    return token_start;
}

// Split command string into arguments
char** split_command(const char* cmd_str, int* argc) {
    if (!cmd_str || !argc) return NULL;
    
    char* cmd_copy = malloc(strlen(cmd_str) + 1);
    strcpy(cmd_copy, cmd_str);
    
    // Count arguments
    *argc = 0;
    char* token = simple_strtok(cmd_copy, " \t\n\r");
    while (token) {
        (*argc)++;
        token = simple_strtok(NULL, " \t\n\r");
    }
    
    if (*argc == 0) {
        free(cmd_copy);
        return NULL;
    }
    
    // Allocate argument array
    char** args = malloc((*argc + 1) * sizeof(char*));
    
    // Reset string and fill arguments
    strcpy(cmd_copy, cmd_str);
    token = simple_strtok(cmd_copy, " \t\n\r");
    int i = 0;
    while (token && i < *argc) {
        args[i] = malloc(strlen(token) + 1);
        strcpy(args[i], token);
        i++;
        token = simple_strtok(NULL, " \t\n\r");
    }
    args[i] = NULL;
    
    free(cmd_copy);
    return args;
}

// Free argument array
void free_args(char** args, int argc) {
    if (!args) return;
    
    for (int i = 0; i < argc; i++) {
        if (args[i]) {
            free(args[i]);
        }
    }
    free(args);
}

// Parse redirections from command string
char* parse_redirections(const char* cmd_str, command_t* cmd) {
    if (!cmd_str || !cmd) return NULL;
    
    char* result = malloc(strlen(cmd_str) + 1);
    strcpy(result, cmd_str);
    
    // Initialize file descriptors array
    cmd->fd_count = 0;
    cmd->fds = malloc(4 * sizeof(file_descriptor_t)); // Support up to 4 redirections
    
    // Look for input redirection <
    char* input_redir = strstr(result, " < ");
    if (!input_redir) input_redir = strstr(result, "<");
    
    if (input_redir) {
        // Find the filename after <
        char* filename_start = input_redir + 1;
        while (*filename_start == ' ' || *filename_start == '\t') filename_start++;
        
        // Find end of filename
        char* filename_end = filename_start;
        while (*filename_end && *filename_end != ' ' && *filename_end != '\t' && 
               *filename_end != '>' && *filename_end != '|') {
            filename_end++;
        }
        
        // Extract filename
        int filename_len = filename_end - filename_start;
        if (filename_len > 0) {
            cmd->fds[cmd->fd_count].filename = malloc(filename_len + 1);
            strncpy(cmd->fds[cmd->fd_count].filename, filename_start, filename_len);
            cmd->fds[cmd->fd_count].filename[filename_len] = '\0';
            cmd->fds[cmd->fd_count].type = REDIR_INPUT;
            cmd->fds[cmd->fd_count].fd = 0; // stdin
            cmd->fds[cmd->fd_count].is_open = 0;
            cmd->fd_count++;
            
            // Remove redirection from command string
            *input_redir = '\0';
            char* new_result = malloc(strlen(result) + 1);
            strcpy(new_result, result);
            strcat(new_result, trim_whitespace(filename_end));
            free(result);
            result = new_result;
        }
    }
    
    // Look for output redirection >
    char* output_redir = strstr(result, " > ");
    if (!output_redir) output_redir = strstr(result, ">");
    
    if (output_redir) {
        // Check for append redirection >>
        int is_append = (output_redir[1] == '>');
        
        // Find the filename after > or >>
        char* filename_start = output_redir + (is_append ? 2 : 1);
        while (*filename_start == ' ' || *filename_start == '\t') filename_start++;
        
        // Find end of filename
        char* filename_end = filename_start;
        while (*filename_end && *filename_end != ' ' && *filename_end != '\t' && 
               *filename_end != '<' && *filename_end != '|') {
            filename_end++;
        }
        
        // Extract filename
        int filename_len = filename_end - filename_start;
        if (filename_len > 0) {
            cmd->fds[cmd->fd_count].filename = malloc(filename_len + 1);
            strncpy(cmd->fds[cmd->fd_count].filename, filename_start, filename_len);
            cmd->fds[cmd->fd_count].filename[filename_len] = '\0';
            cmd->fds[cmd->fd_count].type = is_append ? REDIR_APPEND : REDIR_OUTPUT;
            cmd->fds[cmd->fd_count].fd = 1; // stdout
            cmd->fds[cmd->fd_count].is_open = 0;
            cmd->fd_count++;
            
            // Remove redirection from command string
            *output_redir = '\0';
            char* new_result = malloc(strlen(result) + 1);
            strcpy(new_result, result);
            strcat(new_result, trim_whitespace(filename_end));
            free(result);
            result = new_result;
        }
    }
    
    return trim_whitespace(result);
}

// Parse command from string
command_t* parse_command(const char* cmd_str) {
    if (!cmd_str) return NULL;
    
    command_t* cmd = malloc(sizeof(command_t));
    memset(cmd, 0, sizeof(command_t));
    
    // Check for background execution
    char* input_copy = malloc(strlen(cmd_str) + 1);
    strcpy(input_copy, cmd_str);
    
    // Remove trailing & for background execution
    char* trimmed = trim_whitespace(input_copy);
    if (strlen(trimmed) > 0 && trimmed[strlen(trimmed) - 1] == '&') {
        cmd->background = 1;
        trimmed[strlen(trimmed) - 1] = '\0';
        trimmed = trim_whitespace(trimmed);
    }
    
    // Parse redirections and split into arguments
    char* clean_cmd = parse_redirections(trimmed, cmd);
    
    // Split into arguments
    cmd->args = split_command(clean_cmd, &cmd->argc);
    if (!cmd->args || cmd->argc == 0) {
        free(cmd);
        free(input_copy);
        return NULL;
    }

    cmd->name = cmd->args[0];

    // Alias expansion for pipeline segments (simple commands only).
    // Do not override built-in commands.
    if (find_command(cmd->name) == NULL) {
        char linebuf[256];
        linebuf[0] = '\0';
        for (int a = 0; a < cmd->argc && cmd->args[a]; a++) {
            if (a != 0)
                strcat(linebuf, " ");
            strcat(linebuf, cmd->args[a]);
        }

        char expanded[256];
        int rc = shell_alias_expand_line(linebuf, expanded, (int)sizeof(expanded));
        if (rc == 1) {
            // Reject expansions that would require re-parsing pipeline/redirection.
            for (int x = 0; expanded[x]; x++) {
                if (expanded[x] == '|' || expanded[x] == '<' || expanded[x] == '>' || expanded[x] == '&') {
                    free_args(cmd->args, cmd->argc);
                    free(cmd);
                    free(input_copy);
                    return NULL;
                }
            }

            free_args(cmd->args, cmd->argc);
            cmd->args = split_command(expanded, &cmd->argc);
            if (!cmd->args || cmd->argc == 0) {
                free(cmd);
                free(input_copy);
                return NULL;
            }
            cmd->name = cmd->args[0];
        }
    }
    cmd->type = PIPELINE_CMD_SIMPLE;
    
    free(input_copy);
    return cmd;
}

// Parse pipeline from input string
pipeline_t* parse_pipeline(const char* input) {
    if (!input) return NULL;
    
    pipeline_t* pipeline = malloc(sizeof(pipeline_t));
    memset(pipeline, 0, sizeof(pipeline_t));
    
    // Count commands
    pipeline->command_count = count_pipeline_commands(input);
    
    // Split by pipe character
    char* input_copy = malloc(strlen(input) + 1);
    strcpy(input_copy, input);
    
    char* token = simple_strtok(input_copy, "|");
    command_t* prev_cmd = NULL;
    
    while (token) {
        char* trimmed = trim_whitespace(token);
        if (strlen(trimmed) > 0) {
            command_t* cmd = parse_command(trimmed);
            if (cmd) {
                if (prev_cmd) {
                    prev_cmd->next = cmd;
                    cmd->prev = prev_cmd;
                } else {
                    pipeline->first = cmd;
                }
                prev_cmd = cmd;
            }
        }
        token = simple_strtok(NULL, "|");
    }
    
    free(input_copy);
    return pipeline;
}

// Free command structure
void free_command(command_t* cmd) {
    if (!cmd) return;
    
    if (cmd->args) {
        free_args(cmd->args, cmd->argc);
    }
    
    // Free file descriptors
    for (int i = 0; i < cmd->fd_count; i++) {
        if (cmd->fds[i].filename) {
            free(cmd->fds[i].filename);
        }
    }
    
    free(cmd);
}

// Free pipeline structure
void free_pipeline(pipeline_t* pipeline) {
    if (!pipeline) return;
    
    command_t* cmd = pipeline->first;
    while (cmd) {
        command_t* next = cmd->next;
        free_command(cmd);
        cmd = next;
    }
    
    free(pipeline);
}

// Allocate file descriptor
int allocate_file_descriptor(void) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (!g_file_descriptors[i].is_open) {
            g_file_descriptors[i].is_open = 1;
            return i;
        }
    }
    return -1;
}

// Get file descriptor
file_descriptor_t* get_file_descriptor(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    return &g_file_descriptors[fd];
}

// Close file descriptor
void close_fd(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return;
    
    file_descriptor_t* desc = &g_file_descriptors[fd];
    if (desc->filename) {
        free(desc->filename);
        desc->filename = NULL;
    }
    desc->is_open = 0;
    desc->type = REDIR_NONE;
    desc->pipe_fd = -1;
}

// Read file content for input redirection
char* read_file_for_input_redirection(const char* filename) {
    if (!filename) return NULL;
    
    // Try to read the file using the filesystem
    int fd = open(filename, EYNFS_READ);
    if (fd == -1) {
        printf("Error: Cannot open file '%s' for input redirection.\n", filename);
        return NULL;
    }
    
    // Read file content
    char* buffer = malloc(8192); // 8KB buffer
    if (!buffer) {
        close(fd);
        return NULL;
    }
    
    int bytes_read = read(fd, buffer, 8191);
    close(fd);
    
    if (bytes_read < 0) {
        free(buffer);
        printf("Error: Cannot read file '%s' for input redirection.\n", filename);
        return NULL;
    }
    
    buffer[bytes_read] = '\0';
    return buffer;
}

// Execute simple command with output redirection
int execute_simple_command(command_t* cmd) {
    if (!cmd || !cmd->name) return -1;
    
    // Handle input redirection
    char* input_data = NULL;
    for (int i = 0; i < cmd->fd_count; i++) {
        if (cmd->fds[i].type == REDIR_INPUT && cmd->fds[i].filename) {
            // Read file content for input redirection
            input_data = read_file_for_input_redirection(cmd->fds[i].filename);
            if (input_data) {
                // Store input data for command to use
                g_pipeline_input_data = input_data;
            }
            break;
        }
    }
    
    // Check for output redirection
    int has_output_redirect = 0;
    for (int i = 0; i < cmd->fd_count; i++) {
        if (cmd->fds[i].type == REDIR_OUTPUT || cmd->fds[i].type == REDIR_APPEND) {
            has_output_redirect = 1;
            break;
        }
    }
    
    // Build command string
    char cmd_str[512] = "";
    for (int i = 0; i < cmd->argc && cmd->args[i]; i++) {
        if (i > 0) strcat(cmd_str, " ");
        strcat(cmd_str, cmd->args[i]);
    }
    
    if (has_output_redirect) {
        // Use existing shell redirection mechanism
        start_shell_redirect();
        
        // Execute command
        shell_cmd_handler_t handler = find_command(cmd->name);
        if (handler) {
            handler(cmd_str);
        } else {
            printf("Command not found: %s\n", cmd->name);
        }
        
        // Stop redirection and write to file
        stop_shell_redirect();
        
        // Handle output redirection
        for (int i = 0; i < cmd->fd_count; i++) {
            if (cmd->fds[i].type == REDIR_OUTPUT || cmd->fds[i].type == REDIR_APPEND) {
                int is_append = (cmd->fds[i].type == REDIR_APPEND);
                int result;
                
                if (is_append) {
                    result = append_output_to_file(shell_redirect_buf, strlen(shell_redirect_buf), cmd->fds[i].filename, 0);
                } else {
                    result = write_output_to_file(shell_redirect_buf, strlen(shell_redirect_buf), cmd->fds[i].filename, 0);
                }
                
                if (result == 0) {
                    printf("Output %s to '%s' successfully.\n", 
                           is_append ? "appended" : "written", cmd->fds[i].filename);
                } else {
                    printf("Error %s to file '%s'.\n", 
                           is_append ? "appending" : "writing", cmd->fds[i].filename);
                }
                break;
            }
        }
    } else {
        // No output redirection, execute normally
        shell_cmd_handler_t handler = find_command(cmd->name);
        if (handler) {
            handler(cmd_str);
        } else {
            printf("Command not found: %s\n", cmd->name);
        }
    }
    
    // Clean up input data
    if (input_data) {
        free(input_data);
        g_pipeline_input_data = NULL;
    }
    
    return 0;
}

// Execute command in background
int execute_background_command(command_t* cmd) {
    if (!cmd) return -1;
    
    // Fake it 'till you make it. Not really multitasking, just pretending.
    
    // Build command string
    char cmd_str[512] = "";
    for (int i = 0; i < cmd->argc && cmd->args[i]; i++) {
        if (i > 0) strcat(cmd_str, " ");
        strcat(cmd_str, cmd->args[i]);
    }
    
    // Simulate background execution
    printf("Running command in background: %s\n", cmd_str);
    
    // Execute the command (for now, synchronously)
    shell_cmd_handler_t handler = find_command(cmd->name);
    if (handler) {
        handler(cmd_str);
    } else {
        printf("Command not found: %s\n", cmd->name);
        return -1;
    }
    
    // Add to background process list (simulated PID)
    int simulated_pid = add_background_process(12345, cmd_str); // Simulated PID
    if (simulated_pid >= 0) {
        printf("Background process started with PID %d\n", 12345);
    }
    
    return 0;
}

// Execute command
int execute_command(command_t* cmd) {
    if (!cmd) return -1;
    
    // Check if command should run in background
    if (cmd->background) {
        return execute_background_command(cmd);
    }
    
    return execute_simple_command(cmd);
}

// Execute pipeline (improved implementation)
int execute_pipeline(pipeline_t* pipeline) {
    if (!pipeline || !pipeline->first) return -1;
    
    if (pipeline->command_count == 1) {
        // Single command, no pipes needed
        return execute_command(pipeline->first);
    }
    
    // For simple pipelines (2 commands), capture output and pass as input
    if (pipeline->command_count == 2) {
        command_t* first_cmd = pipeline->first;
        command_t* second_cmd = first_cmd->next;
        
        if (!first_cmd || !second_cmd) return -1;
        
        // Capture output from first command
        start_shell_redirect();
        
        // Execute first command
        char first_cmd_str[512] = "";
        for (int i = 0; i < first_cmd->argc && first_cmd->args[i]; i++) {
            if (i > 0) strcat(first_cmd_str, " ");
            strcat(first_cmd_str, first_cmd->args[i]);
        }
        
        shell_cmd_handler_t first_handler = find_command(first_cmd->name);
        if (first_handler) {
            first_handler(first_cmd_str);
        } else {
            printf("Command not found: %s\n", first_cmd->name);
            stop_shell_redirect();
            return -1;
        }
        
        // Get the output
        stop_shell_redirect();
        char* output = shell_redirect_buf;
        
        // Check if second command is a filter command (like search)
        if (strcmp(second_cmd->name, "search") == 0) {
            // For search command, pass the output as stdin-like input
            // Build command with pattern and mark it as filter mode
            char second_cmd_str[1024] = "";
            strcpy(second_cmd_str, second_cmd->name);
            
            // Add original arguments (the search pattern)
            for (int i = 1; i < second_cmd->argc && second_cmd->args[i]; i++) {
                strcat(second_cmd_str, " ");
                strcat(second_cmd_str, second_cmd->args[i]);
            }
            
            // Add special flag to indicate this is filter mode
            strcat(second_cmd_str, " --filter");
            
            // Store the input data for the search command to use
            g_pipeline_input_data = output;
            
            // Execute second command
            shell_cmd_handler_t second_handler = find_command(second_cmd->name);
            if (second_handler) {
                second_handler(second_cmd_str);
            } else {
                printf("Command not found: %s\n", second_cmd->name);
            }
            
            // Clear the pipeline input data
            g_pipeline_input_data = NULL;
        } else {
            // For other commands, pass output as arguments (original behavior)
            char second_cmd_str[1024] = "";
            strcpy(second_cmd_str, second_cmd->name);
            
            // Add original arguments
            for (int i = 1; i < second_cmd->argc && second_cmd->args[i]; i++) {
                strcat(second_cmd_str, " ");
                strcat(second_cmd_str, second_cmd->args[i]);
            }
            
            // Add the output from first command as additional arguments
            // Split output by whitespace and add each word as an argument
            char* output_copy = malloc(strlen(output) + 1);
            strcpy(output_copy, output);
            
            char* word = simple_strtok(output_copy, " \t\n\r");
            while (word) {
                char* trimmed = trim_whitespace(word);
                if (strlen(trimmed) > 0) {
                    strcat(second_cmd_str, " ");
                    strcat(second_cmd_str, trimmed);
                }
                word = simple_strtok(NULL, " \t\n\r");
            }
            
            free(output_copy);
            
            // Execute second command
            shell_cmd_handler_t second_handler = find_command(second_cmd->name);
            if (second_handler) {
                second_handler(second_cmd_str);
            } else {
                printf("Command not found: %s\n", second_cmd->name);
            }
        }
        
        return 0;
    }
    
    // For complex pipelines (3+ commands), implement proper pipe chaining
    if (pipeline->command_count >= 3) {
        return execute_complex_pipeline(pipeline);
    }
    
    // Fallback for any other cases
    command_t* cmd = pipeline->first;
    while (cmd) {
        execute_command(cmd);
        cmd = cmd->next;
    }
    
    return 0;
}

// Execute complex pipeline with proper pipe chaining
int execute_complex_pipeline(pipeline_t* pipeline) {
    if (!pipeline || pipeline->command_count < 3) return -1;
    
    // Create a chain of pipes for data flow
    char* current_input = NULL;
    command_t* cmd = pipeline->first;
    int cmd_index = 0;
    
    while (cmd) {
        // Start shell redirection to capture output
        start_shell_redirect();
        
        // Build command string
        char cmd_str[1024] = "";
        strcpy(cmd_str, cmd->name);
        
        // Add original arguments
        for (int i = 1; i < cmd->argc && cmd->args[i]; i++) {
            strcat(cmd_str, " ");
            strcat(cmd_str, cmd->args[i]);
        }
        
        // If this is not the first command, add input from previous command
        if (current_input) {
            // For filter commands like search, use --filter mode
            if (strcmp(cmd->name, "search") == 0) {
                strcat(cmd_str, " --filter");
                // Store input data for the command to use
                g_pipeline_input_data = current_input;
            } else {
                // For other commands, append input as arguments
                char* input_copy = malloc(strlen(current_input) + 1);
                strcpy(input_copy, current_input);
                
                char* word = simple_strtok(input_copy, " \t\n\r");
                while (word) {
                    char* trimmed = trim_whitespace(word);
                    if (strlen(trimmed) > 0) {
                        strcat(cmd_str, " ");
                        strcat(cmd_str, trimmed);
                    }
                    word = simple_strtok(NULL, " \t\n\r");
                }
                
                free(input_copy);
            }
        }
        
        // Execute the command
        shell_cmd_handler_t handler = find_command(cmd->name);
        if (handler) {
            handler(cmd_str);
        } else {
            printf("Command not found: %s\n", cmd->name);
            stop_shell_redirect();
            if (current_input) free(current_input);
            return -1;
        }
        
        // Stop redirection and get output
        stop_shell_redirect();
        char* output = shell_redirect_buf;
        
        // Free previous input if it exists
        if (current_input) {
            free(current_input);
        }
        
        // If this is not the last command, store output for next command
        if (cmd->next) {
            current_input = malloc(strlen(output) + 1);
            strcpy(current_input, output);
        } else {
            // Last command - output goes to terminal (already handled by shell_redirect)
            current_input = NULL;
        }
        
        // Clear pipeline input data if it was set
        if (g_pipeline_input_data) {
            g_pipeline_input_data = NULL;
        }
        
        cmd = cmd->next;
        cmd_index++;
    }
    
    // Clean up
    if (current_input) {
        free(current_input);
    }
    
    return 0;
}

// Background process management
int add_background_process(int pid, const char* command) {
    for (int i = 0; i < MAX_BACKGROUND_PROCESSES; i++) {
        if (!g_background_processes[i].active) {
            g_background_processes[i].pid = pid;
            g_background_processes[i].command = malloc(strlen(command) + 1);
            strcpy(g_background_processes[i].command, command);
            g_background_processes[i].status = 0;
            g_background_processes[i].active = 1;
            return i;
        }
    }
    return -1;
}

int remove_background_process(int pid) {
    for (int i = 0; i < MAX_BACKGROUND_PROCESSES; i++) {
        if (g_background_processes[i].active && g_background_processes[i].pid == pid) {
            if (g_background_processes[i].command) {
                free(g_background_processes[i].command);
            }
            g_background_processes[i].active = 0;
            return 0;
        }
    }
    return -1;
}

background_process_t* get_background_process(int pid) {
    for (int i = 0; i < MAX_BACKGROUND_PROCESSES; i++) {
        if (g_background_processes[i].active && g_background_processes[i].pid == pid) {
            return &g_background_processes[i];
        }
    }
    return NULL;
}

void list_background_processes(void) {
    printf("Background processes:\n");
    int count = 0;
    for (int i = 0; i < MAX_BACKGROUND_PROCESSES; i++) {
        if (g_background_processes[i].active) {
            printf("  [%d] %s (PID: %d)\n", 
                   g_background_processes[i].pid,
                   g_background_processes[i].command,
                   g_background_processes[i].pid);
            count++;
        }
    }
    if (count == 0) {
        printf("  No background processes\n");
    }
}

void wait_for_background_process(int pid) {
    background_process_t* proc = get_background_process(pid);
    if (proc) {
        // Just remove process for now
        printf("Waiting for process %d (%s)...\n", pid, proc->command);
        remove_background_process(pid);
    }
}

void wait_for_all_background_processes(void) {
    for (int i = 0; i < MAX_BACKGROUND_PROCESSES; i++) {
        if (g_background_processes[i].active) {
            wait_for_background_process(g_background_processes[i].pid);
        }
    }
}
