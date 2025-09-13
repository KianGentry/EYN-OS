#include <native_exec.h>
#include <shell_command_info.h>
#include <string.h>
#include <util.h>
#include <vga.h>

void native_run_command(string arg);

// Parse filename from command arguments
static char* parse_filename(string arg) {
    static char filename[64];
    uint8 i = 0;
    
    // Skip command name
    while (arg[i] && arg[i] != ' ') i++;
    while (arg[i] && arg[i] == ' ') i++;
    
    if (!arg[i]) {
        return NULL; // No filename provided
    }
    
    uint8 j = 0;
    while (arg[i] && arg[i] != ' ' && j < 63) {
        filename[j++] = arg[i++];
    }
    filename[j] = 0;
    
    return filename;
}

void native_run_command(string arg) {
    char* filename = parse_filename(arg);
    
    if (!filename) {
        printf("Usage: nrun <program.eyn>\n");
        printf("Native execution of EYN programs with kernel API access\n");
        return;
    }
    
    exec_result_t result = native_execute_program(filename);
    
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

REGISTER_SHELL_COMMAND(nrun, "nrun", native_run_command, CMD_STREAMING, 
    "Native execution of EYN programs with kernel API access.\nUsage: nrun <program.eyn>", 
    "nrun test.eyn");
