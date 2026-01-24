#include <misc/types.h>
#include <string.h>
#include <system.h>
#include <shell.h>
#include <util.h>
#include <kb.h>
#include <math.h>
#include <multiboot.h>
#include <utilities/shell/shell_command_info.h>
#include <utilities/shell/alias.h>
#include <utilities/shell/pipeline.h>
#include <utilities/shell/fs_commands.h>
#include <utilities/shell/run_command.h>
#include <utilities/shell/shell_commands.h>
#include <utilities/assemble.h>
#include <fs/vfs.h>
#include <cpu/user_elf.h>
#include <vga.h>
#include <fat32.h>
#define COMMAND_HASH_SIZE 256
typedef struct {
    const char* name;                 // command name key
    shell_cmd_handler_t handler;      // command handler value
} command_hash_entry_t;
static command_hash_entry_t g_command_hash_table[COMMAND_HASH_SIZE];
static int g_command_hash_initialized = 0;
static int g_command_hash_disabled = 0; // Fallback to linear search when table would be full

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

#include <watchdog.h>
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
void spam_cmd(string arg);
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
void sort_cmd(string arg);
void game_cmd(string arg);
void ls_cmd(string arg);
void cd(string arg);
void makedir(string arg);
void copy_cmd(string arg);
void move_cmd(string arg);
void format_cmd_handler(string arg);
void history_cmd(string arg);
void run_cmd(string arg);
void handler_cmd(string arg);

// Auto-run support: if a command isn't recognized, search for a matching .uelf and run it.
static int has_slash(const char* s) {
    for (int i = 0; s && s[i]; i++) if (s[i] == '/') return 1;
    return 0;
}

static int ends_with_uelf(const char* s) {
    if (!s) return 0;
    int n = (int)strlen(s);
    if (n < 5) return 0;
    return strcmp(s + (n - 5), ".uelf") == 0;
}

typedef struct {
    uint8 drive;
    const char* target_name;
    char* out_path;
    int out_cap;
    int found;
    char cur_dir[128];
    char dirs[48][64];
    int ndirs;
} uelf_find_ctx_t;

static int uelf_list_cb(const char* name, int is_dir, uint32 size, void* user) {
    (void)size;
    uelf_find_ctx_t* ctx = (uelf_find_ctx_t*)user;
    if (!name || !name[0]) return 0;

    if (!is_dir) {
        if (strcmp(name, ctx->target_name) == 0) {
            // Build absolute path.
            if (strcmp(ctx->cur_dir, "/") == 0)
                snprintf(ctx->out_path, (size_t)ctx->out_cap, "/%s", name);
            else
                snprintf(ctx->out_path, (size_t)ctx->out_cap, "%s/%s", ctx->cur_dir, name);
            ctx->found = 1;
            return 1; // stop
        }
        return 0;
    }

    // Record subdirectories to traverse.
    if (ctx->ndirs < (int)(sizeof(ctx->dirs) / sizeof(ctx->dirs[0]))) {
        int i = 0;
        while (name[i] && i < 63) {
            ctx->dirs[ctx->ndirs][i] = name[i];
            i++;
        }
        ctx->dirs[ctx->ndirs][i] = 0;
        ctx->ndirs++;
    }
    return 0;
}

static int find_uelf_recursive(uint8 drive, const char* dir, const char* target_name,
                              int depth, char* out_path, int out_cap) {
    if (depth > 12) return 0;
    uelf_find_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.drive = drive;
    ctx.target_name = target_name;
    ctx.out_path = out_path;
    ctx.out_cap = out_cap;
    strncpy(ctx.cur_dir, dir, sizeof(ctx.cur_dir) - 1);
    ctx.cur_dir[sizeof(ctx.cur_dir) - 1] = 0;

    int r = vfs_listdir(drive, dir, uelf_list_cb, &ctx);
    (void)r;
    if (ctx.found) return 1;

    for (int i = 0; i < ctx.ndirs; i++) {
        const char* dname = ctx.dirs[i];
        if (!strcmp(dname, ".") || !strcmp(dname, "..")) continue;

        char child[128];
        if (strcmp(dir, "/") == 0)
            snprintf(child, sizeof(child), "/%s", dname);
        else
            snprintf(child, sizeof(child), "%s/%s", dir, dname);

        if (find_uelf_recursive(drive, child, target_name, depth + 1, out_path, out_cap))
            return 1;
    }
    return 0;
}

static int try_run_unknown_as_uelf(const char* input) {
    if (!input) return 0;

    // Tokenize: cmd + args
    char cmd[64];
    int i = 0;
    while (input[i] && input[i] != ' ' && i < (int)sizeof(cmd) - 1) {
        cmd[i] = input[i];
        i++;
    }
    cmd[i] = 0;
    if (!cmd[0]) return 0;

    // Parse args
    const int MAX_ARGS = 16;
    char arg_buf[MAX_ARGS][64];
    const char* argv[MAX_ARGS];
    int argc = 0;

    while (input[i] && input[i] == ' ') i++;
    while (input[i] && argc < MAX_ARGS) {
        int k = 0;
        while (input[i] && input[i] != ' ' && k < 63) {
            arg_buf[argc][k++] = input[i++];
        }
        arg_buf[argc][k] = 0;
        argv[argc] = arg_buf[argc];
        argc++;
        while (input[i] && input[i] == ' ') i++;
    }

    // Determine target filename (.uelf).
    char target_name[72];
    if (ends_with_uelf(cmd)) {
        strncpy(target_name, cmd, sizeof(target_name) - 1);
        target_name[sizeof(target_name) - 1] = 0;
    } else {
        snprintf(target_name, sizeof(target_name), "%s.uelf", cmd);
    }

    // If the user provided a path, just resolve and try it.
    char abspath[128];
    if (has_slash(cmd)) {
        resolve_path(cmd, shell_current_path, abspath, sizeof(abspath));
        if (!ends_with_uelf(abspath)) {
            char tmp[128];
            snprintf(tmp, sizeof(tmp), "%s.uelf", abspath);
            strncpy(abspath, tmp, sizeof(abspath) - 1);
            abspath[sizeof(abspath) - 1] = 0;
        }
        vfs_stat_t st;
        if (vfs_stat(g_current_drive, abspath, &st) == 0 && st.type == VFS_NODE_FILE) {
            (void)user_elf_run_argv(g_current_drive, abspath, argc, argv);
            return 1;
        }
        return 0;
    }

    // Try current directory first.
    resolve_path(target_name, shell_current_path, abspath, sizeof(abspath));
    vfs_stat_t st;
    if (vfs_stat(g_current_drive, abspath, &st) == 0 && st.type == VFS_NODE_FILE) {
        (void)user_elf_run_argv(g_current_drive, abspath, argc, argv);
        return 1;
    }

    // Fall back to a bounded recursive search from root.
    char found_path[128];
    if (find_uelf_recursive(g_current_drive, "/", target_name, 0, found_path, (int)sizeof(found_path))) {
        (void)user_elf_run_argv(g_current_drive, found_path, argc, argv);
        return 1;
    }
    return 0;
}

// Wrapper function for joke_spam to match expected signature
void spam_cmd(string arg) {
    joke_spam();
}

// Filesystem commands
// Subcommands
void search_size_cmd(string arg);
void search_type_cmd(string arg);
void search_empty_cmd(string arg);
void search_depth_cmd(string arg);
// ...existing code...
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

// Command registration is now handled by the linker section .shellcmds
// All command information is stored in shell_command_info_t structures

// Unified command registration - all commands are now registered via REGISTER_SHELL_COMMAND macro
// The linker section .shellcmds contains all registered commands

// Command loading state
static int streaming_commands_loaded = 0;
static int commands_loaded_count = 0;
// ...existing code...
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
        g_command_hash_table[i].name = NULL;
        g_command_hash_table[i].handler = NULL;
    }
    
    // Build hash table from all registered commands
    size_t num_commands = (__stop_shellcmds - __start_shellcmds);
    // If the number of commands would fill the table completely, disable hashing and fallback to linear search
    if (num_commands >= COMMAND_HASH_SIZE - 1) {
        g_command_hash_disabled = 1;
        g_command_hash_initialized = 1;
        return;
    }
    for (size_t i = 0; i < num_commands; i++) {
        const shell_command_info_t* cmd = &__start_shellcmds[i];
        uint32_t hash = command_hash(cmd->name);
        
        // Simple linear probing for collisions
        while (g_command_hash_table[hash].name != NULL) {
            hash = (hash + 1) % COMMAND_HASH_SIZE;
        }
        g_command_hash_table[hash].name = cmd->name;
        g_command_hash_table[hash].handler = cmd->handler;
    }
    
    g_command_hash_initialized = 1;
}

// Unified command lookup from linker section with O(1) hash optimization
shell_cmd_handler_t find_command(const char* name) {
    // Initialize hash table on first use
    init_command_hash_table();
    if (!name || !*name) return NULL;
    
    // If hashing is disabled due to table saturation, use linear search
    if (g_command_hash_disabled) {
        size_t num_commands = (__stop_shellcmds - __start_shellcmds);
        for (size_t i = 0; i < num_commands; i++) {
            const shell_command_info_t* cmd = &__start_shellcmds[i];
            if (strcmp(cmd->name, name) == 0) {
                return cmd->handler;
            }
        }
        return NULL;
    }
    
    // Try O(1) hash lookup first using open addressing and key comparison
    uint32_t hash = command_hash(name);
    for (int probes = 0; probes < COMMAND_HASH_SIZE; ++probes) {
        const char* slot_name = g_command_hash_table[hash].name;
        if (slot_name == NULL) {
            // Empty slot: not found
            return NULL;
        }
        if (strcmp(slot_name, name) == 0) {
            return g_command_hash_table[hash].handler;
        }
        hash = (hash + 1) % COMMAND_HASH_SIZE;
    }
    // Not found after probing entire table
    
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

static void safe_command_execution(string input, shell_cmd_handler_t handler) {
    // Validate command before execution
    if (!validate_command_arguments((const char*)input)) {
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
    
    // Optional -v flag
    int verbose = 0;
    if (arg[i] == '-' && arg[i+1] == 'v') { verbose = 1; i += 2; while (arg[i] == ' ') i++; }

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
    
    // Set verbosity
    g_asm_verbose = verbose;

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
    // Expand aliases (up to a small depth to avoid loops), then dispatch.
    const char* current = input;
    char expanded_a[256];
    char expanded_b[256];
    char* out = expanded_a;

    for (int depth = 0; depth < 4; depth++) {
        // Parse the command name
        char cmd[256];
        int i = 0;
        while (current[i] && current[i] != ' ' && i < 255) {
            cmd[i] = current[i];
            i++;
        }
        cmd[i] = '\0';

        // If empty input (just Enter), do nothing
        if (cmd[0] == '\0') {
            return;
        }

        // Find and execute built-in commands
        shell_cmd_handler_t handler = find_command(cmd);
        if (handler) {
            safe_command_execution((string)current, handler);
            return;
        }

        // If not a built-in command, try alias expansion
        int rc = shell_alias_expand_line(current, out, 256);
        if (rc == 1) {
            current = out;
            out = (out == expanded_a) ? expanded_b : expanded_a;
            continue;
        }
        if (rc < 0) {
            printf("%cError: alias expansion failed\n", 255, 0, 0);
            return;
        }

        // No alias; stop expanding
        break;
    }

    // If not a built-in command or alias, try auto-running a matching .uelf.
    if (try_run_unknown_as_uelf((string)current))
        return;

    // Command not found
    char cmd2[256];
    int k = 0;
    while (current[k] && current[k] != ' ' && k < 255) {
        cmd2[k] = current[k];
        k++;
    }
    cmd2[k] = '\0';
    printf("%cCommand not found: %s\n", 255, 0, 0, cmd2);
}

// Command safety status functions
int get_command_execution_errors() {
    return command_execution_errors;
}

string get_last_failed_command() {
    static char snapshot[64];
    int i = 0;
    for (; i < 63; i++) {
        char c = (char)last_failed_command[i];
        snapshot[i] = c;
        if (c == '\0') break;
    }
    if (i == 63) snapshot[63] = '\0';
    return snapshot;
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
        // Note shell loop progress for the watchdog
        watchdog_kick("shell-loop");
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
                watchdog_kick("exec-pipeline");
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
                watchdog_kick("exec-line");
                handle_shell_command(ch);
            }
        }

        if (cmdEql(ch, "exit"))
            break;
    }
}
