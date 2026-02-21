#include <fcntl.h>
#include <eynos_syscall.h>

int open(const char* path, int flags, int mode) {
    if (!path) return -1;
    return eyn_syscall3_pii(EYN_SYSCALL_OPEN, path, flags, mode);
}
