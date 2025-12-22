#include <run_command.h>
#include <native_exec.h>
#include <shell_script.h>
#include <util.h>
#include <types.h>
#include <vga.h>
#include <shell_command_info.h>
#include <string.h>
#include <fs_commands.h> // resolve_path, shell_current_path
#include <cpu/user_elf.h>

extern uint8 g_current_drive;

// Function declarations
void run_cmd(string arg);

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

    // check file extension to determine execution method; if missing, treat as flat binary
    const char* ext = strrchr(filename, '.');
    
    exec_result_t result;
    // Resolve relative paths against current shell directory so subdirectories work
    char abspath[128];
    resolve_path(filename, shell_current_path, abspath, sizeof(abspath));
    
    if (ext && strcmp(ext, ".shell") == 0) {
        // execute as shell script
        result = execute_shell_script(abspath);
    } else if (ext && strcmp(ext, ".uelf") == 0) {
        // execute as ring3 ELF using the EYN-OS syscall ABI
        (void)user_elf_run(g_current_drive, abspath);
        return;
    } else if ((ext && strcmp(ext, ".eyn") == 0) || (ext && strcmp(ext, ".bin") == 0) || (ext && strcmp(ext, ".flat") == 0) || !ext) {
        // execute as native program
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
    return malloc(size); // Use standard malloc
}

REGISTER_SHELL_COMMAND(run, "run", run_cmd, CMD_STREAMING, "Run a native program, ring3 ELF, or a shell script.\nUsage: run <program.eyn|program.bin|program.flat|program.uelf|script.shell>", "run user_hello.uelf");