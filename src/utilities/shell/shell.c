#include <types.h>
#include <string.h>
#include <system.h>
#include <shell.h>
#include <util.h>
#include <kb.h>
#include <math.h>
#include <multiboot.h>
#include <vga.h>
#include <fat32.h>
#include <eynfs.h>
#include <shell_commands.h>
#include <fs_commands.h>
#include <fdisk_commands.h>
#include <format_command.h>
#include <write_editor.h>
#include <help_tui.h>
#include <run_command.h>
#include <assemble.h>
#include <subcommands.h>
#include <game_engine.h>
#include <tui.h>
#include <shell_command_info.h>
#include <pipeline.h>

// Circular buffer variables for logging
extern int shell_log_current_line_start;
extern int shell_log_line_count;
extern int shell_log_line_starts[1001];

// LOG_BUF_SIZE is already defined in vga.h

extern int shell_log_active;

// Command hash table for O(1) lookups
#define COMMAND_HASH_SIZE 128
static shell_cmd_handler_t g_command_hash_table[COMMAND_HASH_SIZE];
static int g_command_hash_initialized = 0;

void __stack_chk_fail_local() {
    return;
}

uint8_t g_current_drive = 0;

// get current physical drive from logical drive
uint8_t get_current_physical_drive(void) {
    return g_current_drive;  // g_current_drive is now physical drive
}

// get current logical drive number
uint8_t get_current_logical_drive(void) {
    return ata_physical_to_logical(g_current_drive);
}

// Add a global variable for the current directory path (for now, always root)
char shell_current_path[128] = "/";

// Command execution safety
static volatile int command_execution_errors = 0;
static volatile int last_command_error = 0;
static volatile char last_failed_command[64] = {0};

// Command types are now defined in shell_command_info.h

// Forward declarations for command functions
void init_cmd(string arg);
void memory_cmd(string arg);
void portable_cmd(string arg);
void load_cmd(string arg);
void unload_cmd(string arg);
void status_cmd(string arg);
void search_cmd(string arg);
void process_cmd(string arg);
void error_cmd(string arg);
void validate_cmd(string arg);
void drive_cmd(string arg);
void read_cmd(string arg);
void write_cmd(string arg);
void handler_exit(string arg);
void handler_assemble(string arg);
void joke_spam();
void help_cmd(string arg);
void echo_cmd(string arg);
void ver_cmd(string arg);
void calc_cmd(string arg);
void draw_cmd_handler(string arg);
void log_cmd(string arg);
void lsata_cmd(string arg);
void clear_cmd(string arg);
void catram_cmd(string arg);
void lsram_cmd(string arg);
void random_cmd(string arg);
void sort_cmd(string arg);
void game_cmd(string arg);
void ls_cmd(string arg);
void read_cmd(string arg);
void write_cmd(string arg);
void cd(string arg);
void makedir(string arg);
void deldir(string arg);
void copy_cmd(string arg);
void move_cmd(string arg);
void format_cmd_handler(string arg);
void fdisk_cmd_handler(string arg);
void handler_assemble(string arg);
void history_cmd(string arg);
void run_cmd(string arg);

// Wrapper function for joke_spam to match expected signature
void spam_cmd(string arg) {
    joke_spam();
}

// Filesystem commands
void format_cmd_handler(string arg);
void fdisk_cmd_handler(string arg);
void fscheck(string arg);
void copy_cmd(string arg);
void move_cmd(string arg);
void del(string arg);
void cd(string arg);
void makedir(string arg);
void deldir(string arg);

// Additional commands
void random_cmd(string arg);
void history_cmd(string arg);
void sort_cmd(string arg);
void game_cmd(string arg);
void size(string arg);
void log_cmd(string arg);
void hexdump_cmd(string arg);
void draw_cmd_handler(string arg);

// RAM disk commands
void catram_cmd(string arg);
void lsram_cmd(string arg);

// Subcommands
void search_size_cmd(string arg);
void search_type_cmd(string arg);
void search_empty_cmd(string arg);
void search_depth_cmd(string arg);
void ls_tree_cmd(string arg);
void ls_size_cmd(string arg);
void ls_detail_cmd(string arg);
void fsstat_cmd(string arg);
void cache_stats_cmd(string arg);
void cache_clear_cmd(string arg);
void cache_reset_cmd(string arg);
void blockmap_cmd(string arg);
void debug_superblock_cmd(string arg);
void debug_directory_cmd(string arg);
void help_write_cmd(string arg);
void read_raw_cmd(string arg);
void read_md_cmd(string arg);
void read_image_cmd(string arg);

// Wrapper functions to match existing function names
void ls_cmd(string arg) { ls(arg); }
void clear_cmd(string arg) { clearScreen(); }
void echo_cmd(string arg) { echo(arg); }
void ver_cmd(string arg) { ver(); }
void calc_cmd(string arg) { calc(arg); }
void lsata_cmd(string arg) { lsata(); }
void run_cmd(string arg) { run_command(arg); }

// RAM disk command wrappers
void catram_cmd(string arg) { catram(arg); }
void lsram_cmd(string arg) { lsram(arg); }

typedef void (*shell_cmd_handler_t)(string arg);

// Command registration is now handled by the linker section .shellcmds
// All command information is stored in shell_command_info_t structures

// Unified command registration - all commands are now registered via REGISTER_SHELL_COMMAND macro
// The linker section .shellcmds contains all registered commands

// Command loading state
static int streaming_commands_loaded = 0;
static int commands_loaded_count = 0;

// Command management functions (simplified for unified system)
static void load_streaming_commands() {
    // All commands are now always available through the linker section
    printf("%cAll commands are now always available\n", 0, 255, 0);
}

static void unload_streaming_commands() {
    // Commands are always loaded in the unified system
    printf("%cCommands are always loaded in unified system\n", 255, 165, 0);
}

// Hash function for command names
static uint32_t command_hash(const char* name) {
    uint32_t hash = 5381;
    int c;
    while ((c = *name++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }
    return hash % COMMAND_HASH_SIZE;
}

// Initialize command hash table
static void init_command_hash_table() {
    if (g_command_hash_initialized) return;
    
    // Clear hash table
    for (int i = 0; i < COMMAND_HASH_SIZE; i++) {
        g_command_hash_table[i] = NULL;
    }
    
    // Build hash table from all registered commands
    size_t num_commands = (__stop_shellcmds - __start_shellcmds);
    for (size_t i = 0; i < num_commands; i++) {
        const shell_command_info_t* cmd = &__start_shellcmds[i];
        uint32_t hash = command_hash(cmd->name);
        
        // Simple linear probing for collisions
        while (g_command_hash_table[hash] != NULL) {
            hash = (hash + 1) % COMMAND_HASH_SIZE;
        }
        g_command_hash_table[hash] = cmd->handler;
    }
    
    g_command_hash_initialized = 1;
}

// Unified command lookup from linker section with O(1) hash optimization
shell_cmd_handler_t find_command(const char* name) {
    // Initialize hash table on first use
    init_command_hash_table();
    
    // Try O(1) hash lookup first
    uint32_t hash = command_hash(name);
    uint32_t original_hash = hash;
    
    // Linear probing for collisions
    do {
        if (g_command_hash_table[hash] != NULL) {
            // Verify this is the correct command by checking the original command list
            size_t num_commands = (__stop_shellcmds - __start_shellcmds);
            for (size_t i = 0; i < num_commands; i++) {
                const shell_command_info_t* cmd = &__start_shellcmds[i];
                if (strcmp(cmd->name, name) == 0 && cmd->handler == g_command_hash_table[hash]) {
                    return cmd->handler;
                }
            }
        }
        hash = (hash + 1) % COMMAND_HASH_SIZE;
    } while (hash != original_hash);
    
    // Fallback to linear search if hash lookup fails
    size_t num_commands = (__stop_shellcmds - __start_shellcmds);
    for (size_t i = 0; i < num_commands; i++) {
        const shell_command_info_t* cmd = &__start_shellcmds[i];
        if (strcmp(cmd->name, name) == 0) {
            return cmd->handler;
        }
    }
    return NULL;
}

// Remove unused functions to fix compilation warnings
/*
// Command validation function
static int validate_command_name(const char* name) {
    if (!name) return 0;
    
    // Basic validation - command names should be alphanumeric with underscores
    for (size_t i = 0; name[i]; i++) {
        if (!((name[i] >= 'a' && name[i] <= 'z') ||
              (name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= '0' && name[i] <= '9') ||
              name[i] == '_')) {
            return 0;
        }
    }
    
    return 1;
}

// Get command information
static const shell_command_info_t* get_command_info(const char* name) {
    if (!name) return NULL;
    
    // This would normally look up command metadata
    // For now, return NULL since we don't have a command registry
    return NULL;
}
*/

static int validate_command_arguments(const char* cmd) {
    if (!cmd) return 0;
    
    // Check for null bytes (potential buffer overflow)
    for (int i = 0; cmd[i]; i++) {
        if (cmd[i] == 0) {
            return 0; // Null byte found
        }
    }
    
    // Check for excessive length
    if (strlen(cmd) > 200) {
        return 0; // Command too long
    }
    
    return 1;
}

static void safe_command_execution(const char* input, shell_cmd_handler_t handler) {
    // Validate command before execution
    if (!validate_command_arguments(input)) {
        printf("%c[SAFETY] Invalid command arguments\n", 255, 0, 0);
        command_execution_errors++;
        return;
    }
    
    // Execute the command with full input string
    handler(input);
}

// Handler wrappers for commands needing extra context
void handler_cmd(string arg) {
    printf("%c\nNew recursive shell opened.\n", 0, 255, 0);
    launch_shell(1); // Always launches a new shell at depth 1
}
void handler_exit(string arg) {
    printf("%cGoodbye!\n", 255, 140, 0); // Orange
    // For now, just exit the shell
    asm("hlt");
}

void handler_assemble(string arg) {
    // Parse arguments for assemble command
    char input_file[256] = {0};
    char output_file[256] = {0};
    
    // Skip command name
    int i = 0;
    while (arg[i] && arg[i] != ' ') i++;
    while (arg[i] && arg[i] == ' ') i++;
    
    // Get input file
    int j = 0;
    while (arg[i] && arg[i] != ' ' && j < 255) {
        input_file[j++] = arg[i++];
    }
    input_file[j] = '\0';
    
    // Skip spaces
    while (arg[i] && arg[i] == ' ') i++;
    
    // Get output file
    j = 0;
    while (arg[i] && arg[i] != ' ' && j < 255) {
        output_file[j++] = arg[i++];
    }
    output_file[j] = '\0';
    
    if (!input_file[0] || !output_file[0]) {
        printf("%cUsage: assemble <input.asm> <output.eyn>\n", 255, 255, 255);
        printf("%cExample: assemble test.asm test.eyn\n");
        return;
    }
    
    // Call the actual assembler
    int result = assemble(input_file, output_file);
    if (result == 0) {
        printf("%cAssembly successful: %s -> %s\n", 0, 255, 0, input_file, output_file);
    } else {
        printf("%cAssembly failed with error code %d\n", 255, 0, 0, result);
    }
}

// Enhanced command handling with unified registration
void handle_shell_command(string input) {
    // Parse the input command
    char cmd[256];
    int i = 0;
    while (input[i] && input[i] != ' ' && i < 255) {
        cmd[i] = input[i];
        i++;
    }
    cmd[i] = '\0';
    
    // Find and execute the command using unified lookup
    shell_cmd_handler_t handler = find_command(cmd);
    if (handler) {
        safe_command_execution(input, handler); // Pass full input, not just cmd
        return;
    }
    
    // Command not found
    printf("%cCommand not found: %s\n", 255, 0, 0, cmd);
}

// Command safety status functions
int get_command_execution_errors() {
    return command_execution_errors;
}

string get_last_failed_command() {
    return last_failed_command;
}

// Streaming command management
void load_cmd(string arg) {
    printf("%cLoading streaming commands...\n", 255, 255, 255);
    load_streaming_commands();
}

void unload_cmd(string arg) {
    printf("%cUnloading streaming commands...\n", 255, 255, 255);
    unload_streaming_commands();
}

void status_cmd(string arg) {
    printf("%cCommand System Status:\n", 255, 255, 255);
    printf("%c  System: Unified Command Registration\n", 255, 255, 255);
    printf("%c  Total Commands: %d\n", 255, 255, 255, (int)(__stop_shellcmds - __start_shellcmds));
    printf("%c  Memory Mode: All commands always available\n", 255, 255, 255);
    printf("%c  Registration: Linker-based automatic\n", 255, 255, 255);
}

void launch_shell(int n) {
    // Initialize pipeline system
    init_pipeline_system();
    
    while (1) {
        if (shell_log_active) {
            printf("%c[LOG] ", 0, 255, 0);
        }
        // Print prompt: <drive>:<path>! 
        // convert physical drive to logical drive for display
        uint8 logical_drive = ata_physical_to_logical(g_current_drive);
        if (logical_drive == 0xFF) logical_drive = 0;  // fallback to 0 if mapping fails
        printf("%c%d:%s", 200, 200, 200, logical_drive, shell_current_path); // white for drive:path
        printf("%c! ", 255, 255, 0); // yellow for !
        string ch = readStr_with_history(&g_command_history);
        
        // Initialize dynamic log buffer if logging is active
        if (shell_log_active && shell_log_buf == NULL) {
            // Call the initialization function from vga.c
            extern void init_dynamic_log_buffer(void);
            init_dynamic_log_buffer();
        }
        if (shell_log_active) {
            char logline[256];
            int pos = 0;
            // Only log the user input, not the prompt
            for (int i = 0; ch[i] && pos < 254; i++) {
                logline[pos++] = ch[i];
            }
            logline[pos++] = '\n';
            logline[pos] = '\0';
            for (int k = 0; logline[k] && shell_log_pos < LOG_BUF_SIZE - 1; k++) {
                // Check if we need to start a new line
                if (shell_log_pos == 0 || shell_log_buf[shell_log_pos - 1] == '\n') {
                    shell_log_current_line_start = shell_log_pos;
                }
                
                shell_log_buf[shell_log_pos++] = logline[k];
                
                // If we just added a newline, record the line start
                if (logline[k] == '\n') {
                    shell_log_line_starts[shell_log_line_count] = shell_log_current_line_start;
                    shell_log_line_count++;
                    
                    // Keep only last 1000 lines
                    if (shell_log_line_count > 1000) {
                        // Move buffer content to start, keeping only last 1000 lines
                        int first_line_start = shell_log_line_starts[1];
                        int bytes_to_keep = shell_log_pos - first_line_start;
                        
                        if (bytes_to_keep > 0 && first_line_start < shell_log_pos) {
                            memmove(shell_log_buf, shell_log_buf + first_line_start, bytes_to_keep);
                            shell_log_pos = bytes_to_keep;
                            
                            // Adjust line start positions
                            for (int j = 0; j < 1000; j++) {
                                shell_log_line_starts[j] = shell_log_line_starts[j + 1] - first_line_start;
                            }
                            shell_log_line_count = 1000;
                        }
                    }
                }
            }
            shell_log_buf[shell_log_pos] = '\0';
            shell_log_flush();
        }
        printf("\n");

        // Check if this is a pipeline command
        if (is_pipeline_command(ch)) {
            // Parse and execute pipeline
            pipeline_t* pipeline = parse_pipeline(ch);
            if (pipeline) {
                // Add command to history (only if not empty)
                if (ch && strlen(ch) > 0) {
                    add_to_history(&g_command_history, ch);
                }
                execute_pipeline(pipeline);
                free_pipeline(pipeline);
            } else {
                printf("Failed to parse pipeline command\n");
            }
        } else {
            // Check if this is a sub-command that might contain operators like '>'
            int is_subcommand = 0;
            if (strncmp(ch, "search_size", 11) == 0 || 
                strncmp(ch, "search_type", 11) == 0 ||
                strncmp(ch, "search_empty", 12) == 0 ||
                strncmp(ch, "search_depth", 12) == 0 ||
                strncmp(ch, "read_raw", 8) == 0 ||
                strncmp(ch, "read_md", 7) == 0) {
                is_subcommand = 1;
            }
            
            char cmd[200], filename[64];
            int is_redirect = 0;
            
            // Only parse redirection if it's not a sub-command
            if (!is_subcommand) {
                is_redirect = parse_redirection(ch, cmd, filename);
            }

            if (is_redirect) {
                start_shell_redirect();
                handle_shell_command(cmd);
                int res = write_output_to_file(shell_redirect_buf, strlen(shell_redirect_buf), filename, g_current_drive);
                if (res == 0)
                    printf("%cOutput redirected to '%s' successfully.\n", 0, 255, 0, filename);
                else
                    printf("%cFailed to write file '%s' (error code: %d).\n", 255, 0, 0, filename, res);
                stop_shell_redirect();
            } else {
                // Add command to history (only if not empty)
                if (ch && strlen(ch) > 0) {
                    add_to_history(&g_command_history, ch);
                }
                handle_shell_command(ch);
            }
        }

        if (cmdEql(ch, "exit"))
            break;
    }
}
