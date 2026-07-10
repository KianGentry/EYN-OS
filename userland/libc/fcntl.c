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

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);

    switch (cmd) {
        case F_GETFL:
            /* Kernel doesn't expose full per-fd flags; return 0 (blocking) */
            return 0;
        case F_SETFL: {
            /* Support O_NONBLOCK via FD_SET_NONBLOCK syscall */
            int enable = (arg & O_NONBLOCK) ? 1 : 0;
            return eyn_syscall3_iii(EYN_SYSCALL_FD_SET_NONBLOCK, fd, enable, 0);
        }
        default:
            /* Unsupported command */
            return -1;
    }
}
