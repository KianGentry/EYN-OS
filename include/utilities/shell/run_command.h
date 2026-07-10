#ifndef RUN_COMMAND_H
#define RUN_COMMAND_H

#include <misc/types.h>

void run_command(string ch);

// Process management functions
int get_process_count();
int get_process_isolation_status();
void* user_malloc(uint32 size);

// Process structure
typedef struct {
    uint32 pid;
    uint32 code_start;
    uint32 code_size;
    uint32 stack_start;
    uint32 stack_size;
    uint32 heap_start;
    uint32 heap_size;
    uint32 entry_point;
    char name[32];
    int active;
    int user_mode;
} process_t;

process_t* get_process_by_id(uint32 pid);

#endif