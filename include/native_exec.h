#ifndef NATIVE_EXEC_H
#define NATIVE_EXEC_H

#include <types.h>
#include <eyn_exe_format.h>

// Process execution context
typedef struct {
    uint32 pid;                 // Process ID
    uint32 code_start;          // Code section start address
    uint32 code_size;           // Code section size
    uint32 data_start;          // Data section start address
    uint32 data_size;           // Data section size
    uint32 stack_start;         // Stack start address
    uint32 stack_size;          // Stack size
    uint32 entry_point;         // Entry point address
    uint32 esp;                 // Stack pointer
    uint32 eip;                 // Instruction pointer
    uint32 eax, ebx, ecx, edx;  // General purpose registers
    uint32 esi, edi, ebp;       // Additional registers
    uint32 eflags;              // Flags register
    uint8 active;               // Process active flag
    char name[64];              // Process name
} native_process_t;

// Execution result
typedef enum {
    EXEC_SUCCESS = 0,
    EXEC_ERROR_INVALID_FORMAT,
    EXEC_ERROR_MEMORY_ALLOC,
    EXEC_ERROR_INVALID_ENTRY,
    EXEC_ERROR_EXECUTION_FAILED,
    EXEC_ERROR_PROCESS_TERMINATED
} exec_result_t;

// Function declarations
void native_exec_init(void);
exec_result_t native_execute_program(const char* filename);
exec_result_t native_load_program(const char* filename, native_process_t* process);
exec_result_t native_run_process(native_process_t* process);
void native_cleanup_process(native_process_t* process);

// Process management
native_process_t* native_get_current_process(void);
void native_set_current_process(native_process_t* process);
int native_get_process_count(void);

// Process lifecycle APIs for scheduler integration
exec_result_t native_spawn(const char* filename, uint32* out_pid);
void native_exit(int code);

// Memory management for user programs
void* native_user_alloc(uint32 size);
void native_user_free(void* ptr);
int native_validate_user_memory(uint32 addr, uint32 size);

// User mode execution support
void native_switch_to_user_mode(native_process_t* process);
void native_switch_to_kernel_mode(void);

#endif // NATIVE_EXEC_H
