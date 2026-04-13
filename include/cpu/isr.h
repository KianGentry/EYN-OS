#include <misc/types.h>
#include "multiboot.h"

#ifndef ISR_H
#define ISR_H

/* ISRs reserved for CPU exceptions */
void isr0();
void isr1();
void isr2();
void isr3();
void isr4();
void isr5();
void isr6();
void isr7();
void isr8();
void isr9();
void isr10();
void isr11();
void isr12();
void isr13();
void isr14();
void isr15();
void isr16();
void isr17();
void isr18();
void isr19();
void isr20();
void isr21();
void isr22();
void isr23();
void isr24();
void isr25();
void isr26();
void isr27();
void isr28();
void isr29();
void isr30();
void isr31();

extern string exception_messages[32];

extern void isr_install();

// Error tracking functions
int get_system_error_count();
int get_last_error_code();
uint32 get_last_error_eip();

// Register state as laid out by syscall ISR stub (starting at saved EDI)
typedef struct regs_t {
    uint32 edi;
    uint32 esi;
    uint32 ebp;
    uint32 esp;      // value prior to pusha (not current ESP)
    uint32 ebx;
    uint32 edx;
    uint32 ecx;
    uint32 eax;
    uint32 int_no;
    uint32 err_code;
    uint32 eip;
    uint32 cs;
    uint32 eflags;
    uint32 useresp;  // present only if switching rings
    uint32 ss;       // present only if switching rings
} regs_t;

// Syscall C dispatcher
uint32 syscall_dispatch(regs_t* r);

// Reset the user-task file descriptor table (used when starting/ending ring3 tasks).
void syscall_reset_user_fds(void);

// Toggle whether user FD table should be preserved across SYSCALL_EXIT and
// subsequent user_elf_run_argv launches. Used by shell pipelines.
void syscall_set_user_fd_inherit_mode(int enabled);
int syscall_get_user_fd_inherit_mode(void);

// Configure fd numbers backing user stdin/stdout/stderr for the next/active task.
void syscall_set_user_stdio_fds(int stdin_fd, int stdout_fd, int stderr_fd);
void syscall_reset_user_stdio_fds(void);

// Kernel-side helpers for creating/closing user FD entries for shell plumbing.
int syscall_kernel_pipe_create(int* out_read_fd, int* out_write_fd);
int syscall_kernel_close_user_fd(int fd);
int syscall_kernel_set_user_fd_nonblock(int fd, int enabled);
int syscall_kernel_set_user_pipe_spool(int fd, int enabled);

// Reset the user-task EYNFS streaming-writer handles (used when starting/ending ring3 tasks).
void syscall_reset_user_streams(void);

// Reset/cleanup user-task GUI resources (created tiles + title strings).
void syscall_reset_user_guis(void);

// CPU exception C dispatcher (called from src/cpu/isr.asm)
void isr_dispatch(regs_t* regs);

#if defined(EYNOS_ARCH_AMD64)
/* amd64 native trap-frame views produced by assembly entry stubs. */
typedef struct amd64_interrupt_frame_t {
    uint64 vector;
    uint64 error_code;
    uint64 rip;
    uint64 cs;
    uint64 rflags;
} amd64_interrupt_frame_t;

typedef struct amd64_syscall_frame_t {
    uint64 syscall_no;
    uint64 arg1;
    uint64 arg2;
    uint64 arg3;
    uint64 arg4;
    uint64 arg5;
    uint64 rip;
    uint64 cs;
    uint64 rflags;
} amd64_syscall_frame_t;

void isr_amd64_dispatch_frame(const amd64_interrupt_frame_t* frame);
uint64 syscall_dispatch_amd64_frame(const amd64_syscall_frame_t* frame);
#endif

#endif
