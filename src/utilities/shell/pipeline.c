#include <pipeline.h>
#include <shell.h>
#include <shell_command_info.h>
#include <utilities/shell/shell_args.h>
#include <fs_commands.h>
#include <util.h>
#include <vga.h>
#include <string.h>
#include <eynfs.h>
#include <misc/types.h>
#include <utilities/shell/alias.h>
#include <context.h>
#include <misc/sched.h>
#include <cpu/isr.h>
#include <serial.h>
#include <fs/vfs.h>
#include <cpu/user_elf.h>

// Temporary runtime diagnostics for pipeline debugging.
#ifndef PIPELINE_DEBUG
#define PIPELINE_DEBUG 0
#endif

#if PIPELINE_DEBUG
#define PIPE_DBG(fmt, ...) do { \
    char _pipe_dbg_buf[256]; \
    int _pipe_dbg_n = snprintf(_pipe_dbg_buf, sizeof(_pipe_dbg_buf), "[PIPEDBG] " fmt "\n", ##__VA_ARGS__); \
    if (_pipe_dbg_n > 0) { \
        if (_pipe_dbg_n > (int)sizeof(_pipe_dbg_buf)) _pipe_dbg_n = (int)sizeof(_pipe_dbg_buf); \
        serial_write(SERIAL_COM1, _pipe_dbg_buf, _pipe_dbg_n); \
        printf("%s", _pipe_dbg_buf); \
    } \
} while (0)
#else
#define PIPE_DBG(fmt, ...) do { } while (0)
#endif

// Global variables
file_descriptor_t g_file_descriptors[MAX_FDS];
background_process_t g_background_processes[MAX_BACKGROUND_PROCESSES];
int g_next_fd = 3;  // Start after stdin, stdout, stderr
int g_next_bg_pid = 1;
char* g_pipeline_input_data = NULL;  // Pipeline input data for filter commands

typedef struct {
    pipeline_t* pipeline;
    command_t* next_cmd;
    int current_input_fd;
    int pending_close_in_fd;
    int pending_close_out_fd;
    int active;
} pipeline_runtime_t;

static pipeline_runtime_t g_pipeline_rt = {0};

static void pipeline_restore_stdio_defaults(void);

extern uint8 g_current_drive;

static int pipeline_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

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

    if (!*str) return str;
    
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

static void trim_whitespace_inplace(char* str) {
    if (!str) return;

    char* start = str;
    while (*start && is_whitespace(*start)) start++;

    char* end = start + strlen(start);
    while (end > start && is_whitespace(end[-1])) end--;

    size_t len = (size_t)(end - start);
    if (start != str) {
        memmove(str, start, len);
    }
    str[len] = '\0';
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
    if (!pipeline_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return NULL;
    
    char* cmd_copy = malloc(strlen(cmd_str) + 1);
    if (!cmd_copy) return NULL;
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
    if (!args) {
        free(cmd_copy);
        return NULL;
    }
    
    // Reset string and fill arguments
    strcpy(cmd_copy, cmd_str);
    token = simple_strtok(cmd_copy, " \t\n\r");
    int i = 0;
    while (token && i < *argc) {
        args[i] = malloc(strlen(token) + 1);
        if (!args[i]) {
            free_args(args, i);
            free(cmd_copy);
            return NULL;
        }
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
    if (!pipeline_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return NULL;

    char* result = malloc(strlen(cmd_str) + 1);
    if (!result) return NULL;
    strcpy(result, cmd_str);

    // Initialize file descriptors array
    cmd->fd_count = 0;
    cmd->fds = malloc(4 * sizeof(file_descriptor_t)); // Support up to 4 redirections
    if (cmd->fds) {
        memset(cmd->fds, 0, 4 * sizeof(file_descriptor_t));
    }
    
    // Look for input redirection <
    char* input_redir = strstr(result, " < ");
    if (!input_redir) input_redir = strstr(result, "<");
    
    if (input_redir && cmd->fds && cmd->fd_count < 4) {
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
            char* filename = malloc(filename_len + 1);
            if (filename) {
                strncpy(filename, filename_start, filename_len);
                filename[filename_len] = '\0';

                const char* remainder = filename_end;
                while (*remainder && is_whitespace(*remainder)) remainder++;

                size_t prefix_len = (size_t)(input_redir - result);
                size_t remainder_len = strlen(remainder);
                size_t new_cap = prefix_len + remainder_len + 2; // optional space + NUL
                char* new_result = malloc(new_cap);
                if (new_result) {
                    memcpy(new_result, result, prefix_len);
                    new_result[prefix_len] = '\0';
                    if (remainder_len > 0) {
                        size_t cur_len = strlen(new_result);
                        if (cur_len > 0 && !is_whitespace(new_result[cur_len - 1])) {
                            strcat(new_result, " ");
                        }
                        strcat(new_result, remainder);
                    }
                    free(result);
                    result = new_result;

                    cmd->fds[cmd->fd_count].filename = filename;
                    cmd->fds[cmd->fd_count].type = REDIR_INPUT;
                    cmd->fds[cmd->fd_count].fd = 0; // stdin
                    cmd->fds[cmd->fd_count].is_open = 0;
                    cmd->fd_count++;
                } else {
                    free(filename);
                }
            }
        }
    }
    
    // Look for output redirection >
    char* output_redir = strstr(result, " > ");
    if (!output_redir) output_redir = strstr(result, ">");
    
    if (output_redir && cmd->fds && cmd->fd_count < 4) {
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
            char* filename = malloc(filename_len + 1);
            if (filename) {
                strncpy(filename, filename_start, filename_len);
                filename[filename_len] = '\0';

                const char* remainder = filename_end;
                while (*remainder && is_whitespace(*remainder)) remainder++;

                size_t prefix_len = (size_t)(output_redir - result);
                size_t remainder_len = strlen(remainder);
                size_t new_cap = prefix_len + remainder_len + 2; // optional space + NUL
                char* new_result = malloc(new_cap);
                if (new_result) {
                    memcpy(new_result, result, prefix_len);
                    new_result[prefix_len] = '\0';
                    if (remainder_len > 0) {
                        size_t cur_len = strlen(new_result);
                        if (cur_len > 0 && !is_whitespace(new_result[cur_len - 1])) {
                            strcat(new_result, " ");
                        }
                        strcat(new_result, remainder);
                    }
                    free(result);
                    result = new_result;

                    cmd->fds[cmd->fd_count].filename = filename;
                    cmd->fds[cmd->fd_count].type = is_append ? REDIR_APPEND : REDIR_OUTPUT;
                    cmd->fds[cmd->fd_count].fd = 1; // stdout
                    cmd->fds[cmd->fd_count].is_open = 0;
                    cmd->fd_count++;
                } else {
                    free(filename);
                }
            }
        }
    }

    trim_whitespace_inplace(result);
    return result;
}

// Parse command from string
command_t* parse_command(const char* cmd_str) {
    if (!cmd_str) return NULL;
    if (!pipeline_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return NULL;

    command_t* cmd = malloc(sizeof(command_t));
    if (!cmd) return NULL;
    memset(cmd, 0, sizeof(command_t));
    
    // Check for background execution
    char* input_copy = malloc(strlen(cmd_str) + 1);
    if (!input_copy) {
        free(cmd);
        return NULL;
    }
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
    if (!clean_cmd) {
        if (cmd->fds) free(cmd->fds);
        free(cmd);
        free(input_copy);
        return NULL;
    }
    
    // Split into arguments
    cmd->args = split_command(clean_cmd, &cmd->argc);
    free(clean_cmd);
    if (!cmd->args || cmd->argc == 0) {
        if (cmd->fds) {
            for (int x = 0; x < cmd->fd_count; x++) {
                if (cmd->fds[x].filename) free(cmd->fds[x].filename);
            }
            free(cmd->fds);
        }
        free(cmd);
        free(input_copy);
        return NULL;
    }

    cmd->name = cmd->args[0];

    // Alias expansion is intentionally deferred to handle_shell_command() to
    // keep pipeline parsing deterministic and avoid parse-time stage loss.
    cmd->type = PIPELINE_CMD_SIMPLE;
    
    free(input_copy);
    return cmd;
}

// Parse pipeline from input string
pipeline_t* parse_pipeline(const char* input) {
    if (!input) return NULL;
    if (!pipeline_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return NULL;
    PIPE_DBG("parse input='%s'", input);

    pipeline_t* pipeline = malloc(sizeof(pipeline_t));
    if (!pipeline) return NULL;
    memset(pipeline, 0, sizeof(pipeline_t));
    
    // Count parsed commands from actual segments (do not trust delimiter count
    // if a segment later fails to parse).
    pipeline->command_count = 0;
    
    // Split by pipe character
    char* input_copy = malloc(strlen(input) + 1);
    if (!input_copy) {
        free(pipeline);
        return NULL;
    }
    strcpy(input_copy, input);
    
    command_t* prev_cmd = NULL;

    // Use local in-place splitting instead of simple_strtok(). The tokenizer
    // used by split_command() shares global state, which previously clobbered
    // pipeline tokenization after the first stage.
    char* segment = input_copy;
    while (segment && *segment) {
        char* sep = segment;
        while (*sep && *sep != '|') sep++;
        int had_pipe = (*sep == '|');
        if (had_pipe) {
            *sep = '\0';
        }

        char* trimmed = trim_whitespace(segment);
        PIPE_DBG("segment raw='%s' trimmed='%s' had_pipe=%d", segment, trimmed, had_pipe);
        if (strlen(trimmed) > 0) {
            command_t* cmd = parse_command(trimmed);
            if (!cmd) {
                PIPE_DBG("segment parse failed: '%s'", trimmed);
                free(input_copy);
                free_pipeline(pipeline);
                return NULL;
            }
            if (prev_cmd) {
                prev_cmd->next = cmd;
                cmd->prev = prev_cmd;
            } else {
                pipeline->first = cmd;
            }
            prev_cmd = cmd;
            pipeline->command_count++;
            PIPE_DBG("segment parsed: cmd='%s' argc=%d total=%d", cmd->name ? cmd->name : "(null)", cmd->argc, pipeline->command_count);
        }

        if (had_pipe) {
            segment = sep + 1;
        } else {
            segment = NULL;
        }
    }
    
    free(input_copy);
    if (!pipeline->first || pipeline->command_count <= 0) {
        PIPE_DBG("parse failed: empty pipeline after parsing");
        free_pipeline(pipeline);
        return NULL;
    }
    PIPE_DBG("parse success: command_count=%d", pipeline->command_count);
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

    if (cmd->fds) {
        free(cmd->fds);
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
    if (!pipeline_ctx_allow(CAP_READ_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) return NULL;
    
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
            if (!pipeline_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return -1;
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
        if (!pipeline_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) return -1;
        // Use existing shell redirection mechanism
        start_shell_redirect();
        
        // Execute through unified shell path (binaries-only resolution).
        handle_shell_command(cmd_str);
        
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
        handle_shell_command(cmd_str);
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
    
    // For now, we'll simulate background execution by running the command
    // and immediately returning. In a real implementation, this would:
    // 1. Fork a new process
    // 2. Execute the command in the child process
    // 3. Return immediately in the parent process
    // 4. Track the background process
    
    // Build command string
    char cmd_str[512] = "";
    for (int i = 0; i < cmd->argc && cmd->args[i]; i++) {
        if (i > 0) strcat(cmd_str, " ");
        strcat(cmd_str, cmd->args[i]);
    }
    
    // Simulate background execution
    printf("Running command in background: %s\n", cmd_str);
    
    // Execute through unified shell path (binaries-only resolution).
    handle_shell_command(cmd_str);

    /* Background launches must not leave fd inheritance or stdio remapping
     * armed for the next foreground command. The child task captures whatever
     * state it needs at spawn time; the shell should always return to its
     * default stdio configuration after dispatch.
     */
    pipeline_restore_stdio_defaults();
    syscall_reset_user_fds();
    
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

static void pipeline_restore_stdio_defaults(void) {
    syscall_reset_user_stdio_fds();
    syscall_set_user_fd_inherit_mode(0);
}

static void pipeline_runtime_reset(void) {
    g_pipeline_rt.pipeline = NULL;
    g_pipeline_rt.next_cmd = NULL;
    g_pipeline_rt.current_input_fd = 0;
    g_pipeline_rt.pending_close_in_fd = -1;
    g_pipeline_rt.pending_close_out_fd = -1;
    g_pipeline_rt.active = 0;
}

static void pipeline_runtime_finish(void) {
    if (g_pipeline_rt.pending_close_out_fd > 2) {
        (void)syscall_kernel_close_user_fd(g_pipeline_rt.pending_close_out_fd);
    }
    if (g_pipeline_rt.pending_close_in_fd > 2) {
        (void)syscall_kernel_close_user_fd(g_pipeline_rt.pending_close_in_fd);
    }
    if (g_pipeline_rt.current_input_fd > 2) {
        (void)syscall_kernel_close_user_fd(g_pipeline_rt.current_input_fd);
    }

    pipeline_restore_stdio_defaults();
    syscall_reset_user_fds();

    if (g_pipeline_rt.pipeline) {
        free_pipeline(g_pipeline_rt.pipeline);
    }
    pipeline_runtime_reset();
}

static int command_has_arg(command_t* cmd, const char* arg) {
    if (!cmd || !arg || !cmd->args) return 0;
    for (int i = 0; i < cmd->argc; ++i) {
        if (cmd->args[i] && strcmp(cmd->args[i], arg) == 0) return 1;
    }
    return 0;
}

static void build_command_string(command_t* cmd, char* out, int out_cap, const char* extra_arg) {
    if (!out || out_cap <= 0) return;
    out[0] = '\0';
    if (!cmd || !cmd->args) return;

    for (int i = 0; i < cmd->argc && cmd->args[i]; ++i) {
        if (i > 0) strncat(out, " ", (size_t)(out_cap - (int)strlen(out) - 1));
        strncat(out, cmd->args[i], (size_t)(out_cap - (int)strlen(out) - 1));
    }

    if (extra_arg && extra_arg[0]) {
        strncat(out, " ", (size_t)(out_cap - (int)strlen(out) - 1));
        strncat(out, extra_arg, (size_t)(out_cap - (int)strlen(out) - 1));
    }
}

static int pipeline_try_spawn_user_program(command_t* cmd, const char* extra_arg) {
    if (!cmd || !cmd->name || !cmd->name[0]) return 0;

    char target[128];
    vfs_stat_t st;

    int n = snprintf(target, sizeof(target), "/binaries/%s", cmd->name);
    if (n <= 0 || n >= (int)sizeof(target)) return 0;
    if (vfs_stat(g_current_drive, target, &st) != 0 || st.type != VFS_NODE_FILE) {
        n = snprintf(target, sizeof(target), "/binaries/%s.uelf", cmd->name);
        if (n <= 0 || n >= (int)sizeof(target)) return 0;
        if (vfs_stat(g_current_drive, target, &st) != 0 || st.type != VFS_NODE_FILE) return 0;
    }

    const char* argv[32];
    int argc = 0;
    for (int i = 1; i < cmd->argc && cmd->args[i] && argc < (int)(sizeof(argv) / sizeof(argv[0])); ++i) {
        argv[argc++] = cmd->args[i];
    }
    if (extra_arg && extra_arg[0] && argc < (int)(sizeof(argv) / sizeof(argv[0]))) {
        argv[argc++] = extra_arg;
    }

    int pid = user_task_spawn_argv(g_current_drive, target, argc, argv);
    if (pid <= 0) return 0;
    (void)user_task_waitpid(pid, NULL, 0);
    return 1;
}

// Execute pipeline (improved implementation)
int execute_pipeline(pipeline_t* pipeline) {
    if (!pipeline || !pipeline->first) return -1;
    if (!pipeline_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return -1;
    PIPE_DBG("exec start command_count=%d", pipeline->command_count);

    if (pipeline->command_count <= 1) {
        PIPE_DBG("exec single command path: '%s'", pipeline->first && pipeline->first->name ? pipeline->first->name : "(null)");
        return execute_command(pipeline->first);
    }

    if (g_pipeline_rt.active) {
        PIPE_DBG("exec rejected: runtime already active");
        return -1;
    }

    syscall_set_user_fd_inherit_mode(1);
    g_pipeline_rt.pipeline = pipeline;
    g_pipeline_rt.next_cmd = pipeline->first;
    g_pipeline_rt.current_input_fd = 0;
    g_pipeline_rt.pending_close_in_fd = -1;
    g_pipeline_rt.pending_close_out_fd = -1;
    g_pipeline_rt.active = 1;

    PIPE_DBG("exec runtime armed; first stage='%s'", g_pipeline_rt.next_cmd && g_pipeline_rt.next_cmd->name ? g_pipeline_rt.next_cmd->name : "(null)");
    return 0;
}

// Execute complex pipeline with proper pipe chaining
int execute_complex_pipeline(pipeline_t* pipeline) {
    return execute_pipeline(pipeline);
}

int pipeline_is_runtime_active(void) {
    return g_pipeline_rt.active;
}

// Resume one or more pending pipeline stages.
// Returns 1 if it consumed work, 0 if no pipeline was active.
int pipeline_resume_pending(void) {
    if (!g_pipeline_rt.active) return 0;

    int stage_idx = 0;
    while (g_pipeline_rt.active && g_pipeline_rt.next_cmd) {
        command_t* cmd = g_pipeline_rt.next_cmd;
        int next_read_fd = -1;
        int next_write_fd = -1;
        int stage_output_fd = 1;

        // Close deferred FDs from the previously completed stage.
        if (g_pipeline_rt.pending_close_out_fd > 2) {
            (void)syscall_kernel_close_user_fd(g_pipeline_rt.pending_close_out_fd);
            PIPE_DBG("resume closed deferred out_fd=%d", g_pipeline_rt.pending_close_out_fd);
            g_pipeline_rt.pending_close_out_fd = -1;
        }
        if (g_pipeline_rt.pending_close_in_fd > 2) {
            (void)syscall_kernel_close_user_fd(g_pipeline_rt.pending_close_in_fd);
            PIPE_DBG("resume closed deferred in_fd=%d", g_pipeline_rt.pending_close_in_fd);
            g_pipeline_rt.pending_close_in_fd = -1;
        }

        PIPE_DBG("resume stage %d cmd='%s' current_input_fd=%d has_next=%d", stage_idx, cmd->name ? cmd->name : "(null)", g_pipeline_rt.current_input_fd, cmd->next ? 1 : 0);

        if (cmd->next) {
            if (syscall_kernel_pipe_create(&next_read_fd, &next_write_fd) != 0) {
                printf("Pipeline: failed to create pipe\n");
                PIPE_DBG("resume stage %d pipe_create failed", stage_idx);
                pipeline_runtime_finish();
                return 1;
            }
            // Sequential stage execution can overrun the in-kernel pipe ring;
            // enable bounded spool fallback for this pipeline link.
            (void)syscall_kernel_set_user_pipe_spool(next_write_fd, 1);
            stage_output_fd = next_write_fd;
            PIPE_DBG("resume stage %d pipe read_fd=%d write_fd=%d", stage_idx, next_read_fd, next_write_fd);
        }

        syscall_set_user_stdio_fds(g_pipeline_rt.current_input_fd, stage_output_fd, stage_output_fd);
        PIPE_DBG("resume stage %d stdio remap in=%d out=%d err=%d", stage_idx, g_pipeline_rt.current_input_fd, stage_output_fd, stage_output_fd);

        // Advance runtime state BEFORE executing; SYSCALL_EXIT may non-locally
        // abort back to shell loop and skip the rest of this function.
        g_pipeline_rt.pending_close_in_fd = g_pipeline_rt.current_input_fd;
        g_pipeline_rt.pending_close_out_fd = stage_output_fd;
        g_pipeline_rt.current_input_fd = cmd->next ? next_read_fd : -1;
        g_pipeline_rt.next_cmd = cmd->next;

        if (g_pipeline_rt.pending_close_out_fd <= 2) g_pipeline_rt.pending_close_out_fd = -1;
        if (g_pipeline_rt.pending_close_in_fd <= 2) g_pipeline_rt.pending_close_in_fd = -1;

        if (g_pipeline_rt.pending_close_in_fd > 2 && cmd->name && strcmp(cmd->name, "search") == 0 && !command_has_arg(cmd, "--stdin")) {
            if (!pipeline_try_spawn_user_program(cmd, "--stdin")) {
                char cmd_str[640];
                build_command_string(cmd, cmd_str, (int)sizeof(cmd_str), "--stdin");
                PIPE_DBG("resume stage %d exec via handle_shell_command: %s", stage_idx, cmd_str);
                handle_shell_command(cmd_str);
            }
        } else {
            PIPE_DBG("resume stage %d exec command path: '%s'", stage_idx, cmd->name ? cmd->name : "(null)");
            if (!pipeline_try_spawn_user_program(cmd, NULL)) {
                (void)execute_command(cmd);
            }
        }

        // If execution returns normally (e.g., kernel-side command), continue
        // to next stage immediately.
        stage_idx++;
    }

    if (g_pipeline_rt.active && !g_pipeline_rt.next_cmd) {
        PIPE_DBG("resume finish pipeline");
        pipeline_runtime_finish();
    }
    return 1;
}

// Background process management
int add_background_process(int pid, const char* command) {
    if (!pipeline_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return -1;
    for (int i = 0; i < MAX_BACKGROUND_PROCESSES; i++) {
        if (!g_background_processes[i].active) {
            g_background_processes[i].pid = pid;
            g_background_processes[i].command = malloc(strlen(command) + 1);
            if (!g_background_processes[i].command) {
                return -1;
            }
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
        // In a real implementation, this would wait for the process to complete
        printf("Waiting for process %d (%s)...\n", pid, proc->command);
        // For now, just remove it
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
