#ifndef LINUX_SYSCALLS_H
#define LINUX_SYSCALLS_H

#include <types.h>
#include <native_exec.h>

// Minimal Linux i386 syscall numbers we will support first
#define __NR_exit         1
#define __NR_read         3
#define __NR_write        4
#define __NR_open         5
#define __NR_close        6
#define __NR_lseek        19
#define __NR_time         13
#define __NR_brk          45
#define __NR_getpid       20
#define __NR_uname        122
#define __NR_fstat        108
#define __NR_gettimeofday 78
#define __NR_clock_gettime 265
#define __NR_set_thread_area 243
#define __NR_exit_group   252

// Dispatcher entry point. regs is array: [eax, ecx, edx, ebx, esp, ebp, esi, edi]
// Returns syscall return value to store in eax.
int linux_syscall_dispatch(native_process_t* proc, uint32 regs[8]);

#endif
