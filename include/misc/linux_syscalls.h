#ifndef LINUX_SYSCALLS_H
#define LINUX_SYSCALLS_H

#include <misc/types.h>
#include <native_exec.h>

// Minimal Linux i386 syscall numbers we will support first
#define __NR_exit         1
#define __NR_fork         2
#define __NR_read         3
#define __NR_write        4
#define __NR_open         5
#define __NR_close        6
#define __NR_waitpid      7
#define __NR_execve       11
#define __NR_lseek        19
#define __NR_getuid       24
#define __NR_getgid       47
#define __NR_geteuid      49
#define __NR_getegid      50
#define __NR_getppid      64
#define __NR_nanosleep    162
#define __NR_rt_sigaction 174
#define __NR_rt_sigprocmask 175
#define __NR_sigaltstack  186
#define __NR_wait4        114
#define __NR_clone        120
#define __NR_time         13
#define __NR_brk          45
#define __NR_getpid       20
#define __NR_gettid       224
#define __NR_futex        240
#define __NR_uname        122
#define __NR_fstat        108
#define __NR_vfork        190
#define __NR_gettimeofday 78
#define __NR_clock_gettime 265
#define __NR_set_thread_area 243
#define __NR_set_tid_address 258
#define __NR_clock_getres 266
#define __NR_clock_nanosleep 267
#define __NR_tgkill       270
#define __NR_exit_group   252

// Epoll syscalls (i386 numbers)
#define __NR_epoll_create 211
#define __NR_epoll_ctl    212
#define __NR_epoll_wait   213

// epoll_ctl operations
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

// epoll_wait event masks
#define EPOLLIN  0x001
#define EPOLLOUT 0x004
#define EPOLLERR 0x008
#define EPOLLHUP 0x010

// Memory-related syscalls (i386 numbers)
#define __NR_mmap         90
#define __NR_munmap       91
#define __NR_mprotect     125
#define __NR_mmap2        192

// Prot flags
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

// Map flags
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

// EYN-OS custom networking syscalls (600-series for now)
#define __NR_net_socket   600   // Create UDP socket: returns socket_id or <0
#define __NR_net_bind     601   // Bind socket to port: (socket_id, port)
#define __NR_net_sendto   602   // Send UDP: (socket_id, dst_ip_str, dst_port, buf, len)
#define __NR_net_recvfrom 603   // Receive UDP: (socket_id, buf, buflen, src_ip_out, src_port_out)
#define __NR_net_close    604   // Close socket: (socket_id)

// Dispatcher entry point. regs is array: [eax, ecx, edx, ebx, esp, ebp, esi, edi]
// Returns syscall return value to store in eax.
int linux_syscall_dispatch(native_process_t* proc, uint32 regs[8]);

// Initialize standard file descriptors (stdin, stdout, stderr) for a Linux process
void linux_init_stdio(native_process_t* proc);

#endif
