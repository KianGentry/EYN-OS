#pragma once

// Low-level EYN-OS syscall ABI (int 0x80).
// eax = syscall number
// ebx/ecx/edx = args 1..3

#ifdef __cplusplus
extern "C" {
#endif

enum {
    EYN_SYSCALL_WRITE  = 1,
    EYN_SYSCALL_EXIT   = 2,
    EYN_SYSCALL_READ   = 3,
    EYN_SYSCALL_OPEN   = 4,
    EYN_SYSCALL_CLOSE  = 5,
    EYN_SYSCALL_GETKEY = 6,
    EYN_SYSCALL_GETDENTS = 7,
};

static inline int eyn_syscall3(int n, int a1, const void* a2, int a3) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(n), "b"(a1), "c"(a2), "d"(a3)
        : "memory"
    );
    return ret;
}

static inline int eyn_syscall3_pii(int n, const void* a1, int a2, int a3) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(n), "b"(a1), "c"(a2), "d"(a3)
        : "memory"
    );
    return ret;
}

static inline int eyn_syscall1(int n, int a1) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(n), "b"(a1)
        : "memory"
    );
    return ret;
}

static inline int eyn_syscall0(int n) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(n)
        : "memory"
    );
    return ret;
}

#ifdef __cplusplus
}
#endif
