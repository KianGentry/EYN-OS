#include <dirent.h>
#include <eynos_syscall.h>

int getdents(int fd, eyn_dirent_t* out, size_t bytes) {
    if (!out) return -1;
    if (bytes > 0x7fffffffU) bytes = 0x7fffffffU;
    return eyn_syscall3(EYN_SYSCALL_GETDENTS, fd, out, (int)bytes);
}
