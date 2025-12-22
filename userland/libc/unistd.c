#include <unistd.h>
#include <eynos_syscall.h>

ssize_t write(int fd, const void* buf, size_t len) {
    if (!buf) return -1;
    if (len > 0x7fffffffU) len = 0x7fffffffU;
    return (ssize_t)eyn_syscall3(EYN_SYSCALL_WRITE, fd, buf, (int)len);
}

ssize_t read(int fd, void* buf, size_t len) {
    if (!buf) return -1;
    if (len > 0x7fffffffU) len = 0x7fffffffU;
    return (ssize_t)eyn_syscall3(EYN_SYSCALL_READ, fd, buf, (int)len);
}

int close(int fd) {
    return eyn_syscall1(EYN_SYSCALL_CLOSE, fd);
}

__attribute__((noreturn)) void _exit(int code) {
    (void)eyn_syscall1(EYN_SYSCALL_EXIT, code);
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

int getkey(void) {
    return eyn_syscall0(EYN_SYSCALL_GETKEY);
}
