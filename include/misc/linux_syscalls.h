#ifndef LINUX_SYSCALLS_H
#define LINUX_SYSCALLS_H

#include <misc/types.h>
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

// EYN-OS custom networking syscalls (600-series for now)
#define __NR_net_socket   600   // Create UDP socket: returns socket_id or <0
#define __NR_net_bind     601   // Bind socket to port: (socket_id, port)
#define __NR_net_sendto   602   // Send UDP: (socket_id, dst_ip_str, dst_port, buf, len)
#define __NR_net_recvfrom 603   // Receive UDP: (socket_id, buf, buflen, src_ip_out, src_port_out)
#define __NR_net_close    604   // Close socket: (socket_id)

// Dispatcher entry point. regs is array: [eax, ecx, edx, ebx, esp, ebp, esi, edi]
// Returns syscall return value to store in eax.
int linux_syscall_dispatch(native_process_t* proc, uint32 regs[8]);

#endif
