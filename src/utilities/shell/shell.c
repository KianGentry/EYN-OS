#include <misc/types.h>
#include <string.h>
#include <system.h>
#include <shell.h>
#include <util.h>
#include <kb.h>
#include <math.h>
#include <multiboot.h>
#include <utilities/shell/shell_command_info.h>
#include <utilities/shell/shell_args.h>
#include <utilities/shell/alias.h>
#include <utilities/shell/pipeline.h>
#include <utilities/shell/fs_commands.h>
#include <utilities/shell/run_command.h>
#include <fs/vfs.h>
#include <cpu/user_elf.h>
#include <vga.h>
#include <fat32.h>
#include <context.h>
#include <misc/sched.h>
#include <stdint.h>
#include <utilities/shell/shell_script.h>
#include <ata.h>
#define COMMAND_HASH_SIZE 256
typedef struct {
    const char* name;                 // command name key
    shell_cmd_handler_t handler;      // command handler value
} command_hash_entry_t;
static command_hash_entry_t g_command_hash_table[COMMAND_HASH_SIZE];
static int g_command_hash_initialized = 0;
static int g_command_hash_disabled = 0; // Fallback to linear search when table would be full
static int g_boot_installer_autorun_done = 0;

static int shell_disk_has_installer_binary(void) {
    uint8 logical_count = ata_get_num_logical_drives();
    for (uint8 logical = 0; logical < logical_count; ++logical) {
        if (!ata_logical_drive_present(logical)) continue;
        uint8 physical = ata_logical_to_physical(logical);
        if (physical == 0xFFu) continue;
        if (vfs_detect(physical) != VFS_FS_EYNFS) continue;

        vfs_stat_t st;
        if (vfs_stat(physical, "/binaries/installer", &st) == 0 && st.type == VFS_NODE_FILE) {
            return 1;
        }
    }
    return 0;
}

static size_t shell_command_count(void) {
    uintptr_t start = (uintptr_t)__start_shellcmds;
    uintptr_t stop = (uintptr_t)__stop_shellcmds;
    if (stop < start) return 0;
    return (size_t)((stop - start) / (uintptr_t)sizeof(shell_command_info_t));
}

void __stack_chk_fail_local(void);

void __stack_chk_fail_local(void) {
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

static int try_run_uelf_at_path(uint8 drive, const char* abspath, int argc, const char* const* argv) {
    if (!abspath || !abspath[0]) return 0;
    vfs_stat_t st;
    if (vfs_stat(drive, abspath, &st) == 0 && st.type == VFS_NODE_FILE) {
        /*
         * Auto-detect file type: ELF magic → run as UELF, '#' first byte
         * → run as shell script, anything else → UELF (legacy behavior).
         */
        uint8 magic[4] = {0, 0, 0, 0};
        vfs_read_file(drive, abspath, magic, 4);
        if (magic[0] == '#') {
            (void)shell_script_run(drive, abspath, argc, argv);
            return 1;
        }
        (void)user_elf_run_argv(drive, abspath, argc, argv);
        return 1;
    }
    return 0;
}

static int shell_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
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

static int try_run_unknown_as_uelf(const shell_args_t* args) {
    if (!args || args->argc == 0) return 0;

    const char* cmd = args->argv[0];
    if (!cmd || !cmd[0]) return 0;

    if (!shell_ctx_allow(CAP_READ_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) return 0;

    int argc = 0;
    const char* argv[SHELL_ARGS_MAX];
    for (uint32 i = 1; i < args->argc && argc < (int)SHELL_ARGS_MAX; i++) {
        argv[argc++] = args->argv[i];
    }

    // Determine candidate filenames.
    // We support both extensionless files (recommended for /binaries, like /bin on Linux)
    // and explicit ".uelf" files for backward compatibility.
    char target_plain[72];
    char target_uelf[72];
    if (ends_with_uelf(cmd)) {
        // If the user typed an explicit .uelf, treat it as the plain name.
        strncpy(target_plain, cmd, sizeof(target_plain) - 1);
        target_plain[sizeof(target_plain) - 1] = 0;
        strncpy(target_uelf, cmd, sizeof(target_uelf) - 1);
        target_uelf[sizeof(target_uelf) - 1] = 0;
    } else {
        strncpy(target_plain, cmd, sizeof(target_plain) - 1);
        target_plain[sizeof(target_plain) - 1] = 0;
        snprintf(target_uelf, sizeof(target_uelf), "%s.uelf", cmd);
    }

    // If the user provided a path, just resolve and try it.
    char abspath[128];
    if (has_slash(cmd)) {
        resolve_path(cmd, shell_current_path, abspath, sizeof(abspath));
        // Try the path as given (extensionless allowed).
        if (try_run_uelf_at_path(g_current_drive, abspath, argc, argv)) return 1;

        // Also try with .uelf appended if the user omitted it.
        if (!ends_with_uelf(abspath)) {
            char tmp[128];
            snprintf(tmp, sizeof(tmp), "%s.uelf", abspath);
            if (try_run_uelf_at_path(g_current_drive, tmp, argc, argv)) return 1;
        }
        return 0;
    }

    // Prefer /binaries first (like /bin). This avoids surprising "current directory" shadowing
    // and makes command resolution deterministic.
    // Note: keep path resolution simple; /binaries is always absolute.
    snprintf(abspath, sizeof(abspath), "/binaries/%s", target_plain);
    if (try_run_uelf_at_path(g_current_drive, abspath, argc, argv)) return 1;
    snprintf(abspath, sizeof(abspath), "/binaries/%s", target_uelf);
    if (try_run_uelf_at_path(g_current_drive, abspath, argc, argv)) return 1;

    // Binaries-only policy: no current-directory or recursive fallback for unknown commands.
    return 0;
}

// Enhanced command handling with unified registration
void handle_shell_command(string input) {
    command_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.caps = CAP_ALL;
    ctx.drive = g_current_drive;
    int ctx_pushed = command_context_push(&ctx);

    // Expand aliases (up to a small depth to avoid loops), then dispatch.
    const char* current = input;
    char expanded_a[256];
    char expanded_b[256];
    char* out = expanded_a;

    for (int depth = 0; depth < 4; depth++) {
        shell_args_t args;
        int prc = shell_args_parse(&args, current);
        if (prc < 0) {
            printf("%cError: too many arguments or command line too long\n", 255, 0, 0);
            goto cleanup;
        }

        // If empty input (just Enter), do nothing
        if (args.argc == 0 || !args.argv[0] || args.argv[0][0] == '\0') {
            goto cleanup;
        }

        // Expand aliases before resolving userland binaries.
        int rc = shell_alias_expand_line(current, out, 256);
        if (rc == 1) {
            current = out;
            out = (out == expanded_a) ? expanded_b : expanded_a;
            continue;
        }
        if (rc < 0) {
            printf("%cError: alias expansion failed\n", 255, 0, 0);
            goto cleanup;
        }

        // No alias; stop expanding
        break;
    }

    // Allow pipeline syntax in all dispatch paths (interactive shell, scripts,
    // and any internal callers), not only in launch_shell().
    if (is_pipeline_command(current)) {
        pipeline_t* pipeline = parse_pipeline(current);
        if (pipeline) {
            watchdog_kick("exec-pipeline");
            (void)execute_pipeline(pipeline);
            // Multi-stage pipeline execution is resumable and transfers
            // ownership of the parsed pipeline to the runtime state.
            if (!pipeline_is_runtime_active()) {
                free_pipeline(pipeline);
            }
        } else {
            printf("Failed to parse pipeline command\n");
        }
        goto cleanup;
    }

    // Resolve and execute userland binaries only.
    shell_args_t unknown_args;
    if (shell_args_parse(&unknown_args, current) == 0 && try_run_unknown_as_uelf(&unknown_args))
        goto cleanup;

    // Command not found
    if (unknown_args.argc > 0 && unknown_args.argv[0])
        printf("%cCommand not found in /binaries: %s\n", 255, 0, 0, unknown_args.argv[0]);
    else
        printf("%cCommand not found in /binaries\n", 255, 0, 0);
cleanup:
    if (ctx_pushed) {
        command_context_pop();
    }
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

void launch_shell(int n) {
    // Initialize pipeline system
    init_pipeline_system();

    int disk_has_installer = shell_disk_has_installer_binary();
    if (!disk_has_installer && vfs_detect(g_current_drive) == VFS_FS_NONE && vfs_detect(VFS_DRIVE_RAM) == VFS_FS_EYNFS) {
        g_current_drive = VFS_DRIVE_RAM;
        strncpy(shell_current_path, "/", sizeof(shell_current_path) - 1);
        shell_current_path[sizeof(shell_current_path) - 1] = '\0';
        printf("%c[installer] defaulting shell drive to RAM:/\n", 140, 220, 255);
    }

    if (!disk_has_installer && !g_boot_installer_autorun_done) {
        vfs_stat_t st;
        if (vfs_stat(VFS_DRIVE_RAM, "/binaries/installer", &st) == 0 && st.type == VFS_NODE_FILE) {
            g_boot_installer_autorun_done = 1;
            printf("%c[installer] launching RAM:/binaries/installer\n", 140, 220, 255);
            (void)user_elf_run_argv(VFS_DRIVE_RAM, "/binaries/installer", 0, NULL);
        }
    }
    
    while (1) {
        // Resume any multi-stage pipeline that was interrupted by user-task
        // exit abort path after a prior stage.
        if (pipeline_resume_pending()) {
            continue;
        }

        // Note shell loop progress for the watchdog
        watchdog_kick("shell-loop");
        if (shell_log_active) {
            if (!shell_ctx_allow(CAP_READ_FS | CAP_WRITE_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) {
                shell_log_active = 0;
            } else {
                printf("%c[LOG] ", 0, 255, 0);
            }
        }
        // Print prompt: <drive>:<path>! 
        // convert physical drive to logical drive for display
        if (g_current_drive == VFS_DRIVE_RAM) {
            printf("%cRAM:%s", 200, 200, 200, shell_current_path);
        } else {
            uint8 logical_drive = ata_physical_to_logical(g_current_drive);
            if (logical_drive == 0xFF) logical_drive = 0;  // fallback to 0 if mapping fails
            printf("%c%d:%s", 200, 200, 200, logical_drive, shell_current_path); // white for drive:path
        }
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
                // execute_pipeline() takes ownership for multi-stage pipelines
                // and may resume across subsequent shell-loop ticks.
                if (execute_pipeline(pipeline) < 0) {
                    free_pipeline(pipeline);
                }
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
                if (!shell_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) {
                    printf("%cRedirection requires filesystem write capability.\n", 255, 0, 0);
                    continue;
                }
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
