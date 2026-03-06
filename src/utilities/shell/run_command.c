#include <run_command.h>
#include <native_exec.h>
#include <util.h>
#include <misc/types.h>
#include <vga.h>
#include <string.h>
#include <fs_commands.h> // resolve_path, shell_current_path
#include <cpu/user_elf.h>
#include <context.h>
#include <misc/sched.h>
#include <fs/vfs.h>
#include <utilities/shell/shell_script.h>

extern uint8 g_current_drive;

static int run_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

void run_command(string arg) {
    // Parse filename
    uint8 i = 0;
    while (arg[i] && arg[i] != ' ') i++;
    while (arg[i] && arg[i] == ' ') i++;
    if (!arg[i]) {
        printf("Usage: run <program.eyn|program.flat|program.bin|script.shell>\n");
        return;
    }
    char filename[64];
    uint8 j = 0;
    while (arg[i] && arg[i] != ' ' && j < 63) filename[j++] = arg[i++];
    filename[j] = 0;

    if (!run_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return;

    // Parse remaining args (space-separated; no quoting support).
    const int MAX_ARGS = 16;
    char arg_buf[MAX_ARGS][64];
    const char* argv[MAX_ARGS];
    int argc = 0;
    while (arg[i] && arg[i] == ' ') i++;
    while (arg[i] && argc < MAX_ARGS) {
        int k = 0;
        while (arg[i] && arg[i] != ' ' && k < 63) {
            arg_buf[argc][k++] = arg[i++];
        }
        arg_buf[argc][k] = 0;
        argv[argc] = arg_buf[argc];
        argc++;
        while (arg[i] && arg[i] == ' ') i++;
    }

    // check file extension to determine execution method; if missing, treat as flat binary
    const char* ext = strrchr(filename, '.');
    
    exec_result_t result;
    // Resolve relative paths against current shell directory so subdirectories work
    char abspath[128];
    resolve_path(filename, shell_current_path, abspath, sizeof(abspath));
    
    if (ext && strcmp(ext, ".shell") == 0) {
        // execute as shell script via the script interpreter
        (void)shell_script_run(g_current_drive, abspath, argc, argv);
        return;
    } else if (ext && strcmp(ext, ".uelf") == 0) {
        // execute as ring3 ELF using the EYN-OS syscall ABI
        (void)user_elf_run_argv(g_current_drive, abspath, argc, argv);
        return;
    } else if ((ext && strcmp(ext, ".eyn") == 0) || (ext && strcmp(ext, ".bin") == 0) || (ext && strcmp(ext, ".flat") == 0)) {
        // execute as native program (explicit native extension)
        result = native_execute_program(abspath);
    } else if (!ext) {
        /*
         * No extension: auto-detect by reading the first 4 bytes.
         * Most /binaries entries are extensionless UELFs.  If the file starts
         * with "\x7fELF", run as ring3 UELF; if it starts with '#' (script
         * comment/shebang), run as a shell script; otherwise fall back to native.
         */
        uint8 magic[4] = {0, 0, 0, 0};
        vfs_read_file(g_current_drive, abspath, magic, 4);
        if (magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
            (void)user_elf_run_argv(g_current_drive, abspath, argc, argv);
            return;
        }
        if (magic[0] == '#') {
            (void)shell_script_run(g_current_drive, abspath, argc, argv);
            return;
        }
        result = native_execute_program(abspath);
    } else {
        printf("Error: Unsupported file format. Use .eyn/.bin/.flat for native programs, .uelf for ring3 ELF, or .shell for scripts\n");
        return;
    }
    
    // Only show errors, not success messages
    switch (result) {
        case EXEC_SUCCESS:
            // Silent success
            break;
            
        case EXEC_ERROR_INVALID_FORMAT:
            printf("Error: Invalid or unsupported file format\n");
            break;
            
        case EXEC_ERROR_MEMORY_ALLOC:
            printf("Error: Memory allocation failed\n");
            break;
            
        case EXEC_ERROR_INVALID_ENTRY:
            printf("Error: Invalid entry point\n");
            break;
            
        case EXEC_ERROR_EXECUTION_FAILED:
            printf("Error: Program execution failed\n");
            break;
            
        case EXEC_ERROR_PROCESS_TERMINATED:
            printf("Program terminated\n");
            break;
            
        default:
            printf("Error: Unknown execution error (%d)\n", result);
            break;
    }
}

// Legacy process management functions kept for compatibility
int get_process_count() {
    return 0; // No legacy processes
}

process_t* get_process_by_id(uint32 pid) {
    return NULL; // No legacy processes
}

int get_process_isolation_status() {
    return 1; // Always enabled with native execution
}

void* user_malloc(uint32 size) {
    if (!run_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return NULL;
    return malloc(size); // Use standard malloc
}
