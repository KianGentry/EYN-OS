#include <fcntl.h>
#include <eynos_syscall.h>
#include <stdarg.h>

int open(const char* path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (!path) return -1;
    return eyn_syscall3_pii(EYN_SYSCALL_OPEN, path, flags, mode);
}

int creat(const char* path, int mode) {
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}
