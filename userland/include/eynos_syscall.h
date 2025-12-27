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

    // GUI / tiling manager integration
    EYN_SYSCALL_GUI_CREATE    = 8,
    EYN_SYSCALL_GUI_SET_TITLE = 9,

    EYN_SYSCALL_GUI_BEGIN      = 10,
    EYN_SYSCALL_GUI_CLEAR      = 11,
    EYN_SYSCALL_GUI_FILL_RECT  = 12,
    EYN_SYSCALL_GUI_DRAW_TEXT  = 13,
    EYN_SYSCALL_GUI_PRESENT    = 14,
    EYN_SYSCALL_GUI_POLL_EVENT = 15,
    EYN_SYSCALL_GUI_WAIT_EVENT = 16,
    EYN_SYSCALL_GUI_ATTACH     = 17,

    EYN_SYSCALL_GUI_DRAW_LINE        = 18,
    EYN_SYSCALL_GUI_GET_CONTENT_SIZE = 19,
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
