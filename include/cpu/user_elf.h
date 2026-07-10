#ifndef USER_ELF_H
#define USER_ELF_H

#include <misc/types.h>

typedef struct regs_t regs_t;

// Minimal ELF32 loader that maps PT_LOAD segments into user space and enters ring3.
// Returns 0 on success (does not return to caller if user mode is entered), -1 on failure.
int user_elf_run(uint8 drive, const char* abspath);

// Run a ring3 ELF32 program with argv. argv strings are copied onto the user stack.
// argc may be 0; argv may be NULL.
// envp may be NULL. On success this does not return (enters user mode).
int user_elf_run_argv(uint8 drive, const char* abspath, int argc, const char* const* argv, const char* const* envp);

// Spawn/wait API used by syscall layer and shell pipeline runtime.
// USER_TASK_WAIT_NOHANG requests non-blocking wait semantics.
#define USER_TASK_WAIT_NOHANG 1

int user_task_spawn_argv(uint8 drive, const char* abspath, int argc, const char* const* argv);
int user_task_spawn_argv_stdio(uint8 drive,
							   const char* abspath,
							   int argc,
							   const char* const* argv,
							   int stdin_fd,
							   int stdout_fd,
							   int stderr_fd,
							   int inherit_mode);
int user_task_waitpid(int pid, int* out_status, int flags);

// Record completion status for the currently running spawned user task.
// Called from syscall/interrupt abort paths that terminate ring3 execution.
void user_task_notify_exit(int status);
int user_task_continue_or_schedule(void);
void user_task_request_schedule(void);
int user_task_poll_scheduler(void);
void user_task_capture_syscall_frame(const regs_t* regs);
int user_task_try_resume_from_syscall(regs_t* regs);
int user_task_try_preempt_from_irq(void* frame);

void user_task_scheduler_tick(void);
void user_task_block_current_sleep_until(uint32 wake_tick);
void user_task_block_current_waitpid(int target_pid);
void user_task_block_current_gui_wait(int gui_handle);
void user_task_unblock_current(void);
int user_task_current_is_blocked(void);
void user_task_wake_waiters_for_pid(int pid);
void user_task_wake_gui_waiters(int gui_handle);

// Return the vterm index that should receive stdout/stderr for the current
// running user task, or -1 if no task is active.
int user_task_get_output_vterm(void);

// Mapping ownership accessors used by abort/cleanup logic.
void user_task_get_current_mapping_state(uint32* base, uint32* pages, uint32* stack_page);
void user_task_set_current_mapping_state(uint32 base, uint32 pages, uint32 stack_page);
void user_task_clear_current_mapping_state(void);

// Queue a signal for a UELF task by PID. Returns 0 on success, -1 on error.
int user_task_queue_signal(int pid, int sig);
// Install a handler for the calling UELF task. Returns 0 on success.
int user_task_set_handler_current(int sig, uintptr handler);
// Restore the saved signal frame for the calling UELF task. Returns 0 on success.
int user_task_sigreturn_current(regs_t* regs);
// Queue a signal to the currently running UELF task. Returns 0 on success.
int user_task_signal_current(int sig);
#endif
