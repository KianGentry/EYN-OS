#ifndef USER_ELF_H
#define USER_ELF_H

#include <misc/types.h>

// Minimal ELF32 loader that maps PT_LOAD segments into user space and enters ring3.
// Returns 0 on success (does not return to caller if user mode is entered), -1 on failure.
int user_elf_run(uint8 drive, const char* abspath);

// Run a ring3 ELF32 program with argv. argv strings are copied onto the user stack.
// argc may be 0; argv may be NULL.
int user_elf_run_argv(uint8 drive, const char* abspath, int argc, const char* const* argv);

// Spawn/wait API used by syscall layer and shell pipeline runtime.
// USER_TASK_WAIT_NOHANG requests non-blocking wait semantics.
#define USER_TASK_WAIT_NOHANG 1

int user_task_spawn_argv(uint8 drive, const char* abspath, int argc, const char* const* argv);
int user_task_waitpid(int pid, int* out_status, int flags);

// Record completion status for the currently running spawned user task.
// Called from syscall/interrupt abort paths that terminate ring3 execution.
void user_task_notify_exit(int status);

#endif
