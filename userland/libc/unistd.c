#include <unistd.h>
#include <eynos_syscall.h>

#include <stdint.h>

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

int writefile(const char* path, const void* buf, size_t len) {
    if (!path || !buf) return -1;
    if (len > 0x7fffffffU) len = 0x7fffffffU;
    return eyn_syscall3_ppi(EYN_SYSCALL_WRITEFILE, path, buf, (int)len);
}

int mkdir(const char* path) {
    if (!path) return -1;
    return eyn_syscall1(EYN_SYSCALL_MKDIR, (int)(uintptr_t)path);
}

int unlink(const char* path) {
    if (!path) return -1;
    return eyn_syscall1(EYN_SYSCALL_UNLINK, (int)(uintptr_t)path);
}

int rmdir(const char* path) {
    if (!path) return -1;
    return eyn_syscall1(EYN_SYSCALL_RMDIR, (int)(uintptr_t)path);
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

int usleep(unsigned int usec) {
    // Cooperative sleep to allow GUI and shell updates.
    (void)eyn_syscall1(EYN_SYSCALL_SLEEP_US, (int)usec);
    return 0;
}

unsigned int sleep(unsigned int seconds) {
    // Best-effort: convert seconds to microseconds.
    unsigned int usec = seconds * 1000000u;
    (void)usleep(usec);
    return 0;
}

int getcwd(char* buf, size_t size) {
    if (!buf || size == 0) return -1;
    if (size > 0x7fffffffU) size = 0x7fffffffU;
    return eyn_syscall3_pii(EYN_SYSCALL_GETCWD, buf, (int)size, 0);
}

int chdir(const char* path) {
    if (!path) return -1;
    return eyn_syscall1(EYN_SYSCALL_CHDIR, (int)(uintptr_t)path);
}

int eynfs_stream_begin(const char* path) {
    if (!path) return -1;
    return eyn_syscall1(EYN_SYSCALL_EYNFS_STREAM_BEGIN, (int)(uintptr_t)path);
}

ssize_t eynfs_stream_write(int handle, const void* buf, size_t len) {
    if (!buf) return -1;
    if (len > 0x7fffffffU) len = 0x7fffffffU;
    return (ssize_t)eyn_syscall3(EYN_SYSCALL_EYNFS_STREAM_WRITE, handle, buf, (int)len);
}

int eynfs_stream_end(int handle) {
    return eyn_syscall1(EYN_SYSCALL_EYNFS_STREAM_END, handle);
}
