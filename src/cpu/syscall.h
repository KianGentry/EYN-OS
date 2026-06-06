/* Minimal syscall number definitions for kernel dispatch.
 * This file is intended to centralize syscall numbers for kernel implementation.
 */
#ifndef SYS_CALL_H
#define SYS_CALL_H

enum {
    SYSCALL_RUN = 96,
    SYSCALL_EXECVE = 97,
    SYSCALL_FORK = 98,
    SYSCALL_VFORK = 99,
};

#endif
