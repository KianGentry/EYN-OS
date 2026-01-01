
#include <stddef.h>
#include <stdint.h>

// EYN-OS syscall ABI: int 0x80
// eax = syscall number
// ebx/ecx/edx = args 1..3

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
};

static inline int eyn_sys_write(int fd, const void* buf, int len) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_WRITE), "b"(fd), "c"(buf), "d"(len)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_read(int fd, void* buf, int len) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_READ), "b"(fd), "c"(buf), "d"(len)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_getkey(void) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_GETKEY)
        : "memory"
    );
    return ret;
}

__attribute__((noreturn))
static inline void eyn_sys_exit(int code) {
    __asm__ __volatile__(
        "int $0x80"
        :
        : "a"(EYN_SYSCALL_EXIT), "b"(code)
        : "memory"
    );
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

static inline size_t eyn_strlen(const char* s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static inline void eyn_write_str(const char* s) {
    if (!s) return;
    (void)eyn_sys_write(1, s, (int)eyn_strlen(s));
}
