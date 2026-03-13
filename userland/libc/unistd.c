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

int dup(int oldfd) {
    return eyn_syscall1(EYN_SYSCALL_DUP, oldfd);
}

int dup2(int oldfd, int newfd) {
    return eyn_syscall3_iii(EYN_SYSCALL_DUP2, oldfd, newfd, 0);
}

int pipe(int pipefd[2]) {
    if (!pipefd) return -1;
    return eyn_syscall1(EYN_SYSCALL_PIPE, (int)(uintptr_t)pipefd);
}

int mkfifo(const char* path, mode_t mode) {
    (void)mode;
    if (!path) return -1;
    return eyn_syscall1(EYN_SYSCALL_MKFIFO, (int)(uintptr_t)path);
}

int fd_set_inherit(int enabled) {
    return eyn_syscall1(EYN_SYSCALL_FD_SET_INHERIT, enabled ? 1 : 0);
}

int fd_set_stdio(int stdin_fd, int stdout_fd, int stderr_fd) {
    return eyn_syscall3_iii(EYN_SYSCALL_FD_SET_STDIO, stdin_fd, stdout_fd, stderr_fd);
}

int fd_set_nonblock(int fd, int enabled) {
    return eyn_syscall3_iii(EYN_SYSCALL_FD_SET_NONBLOCK, fd, enabled ? 1 : 0, 0);
}

int spawn(const char* path, const char* const* argv, int argc) {
    if (!path || argc < 0) return -1;
    return eyn_syscall3_ppi(EYN_SYSCALL_SPAWN,
                            path,
                            (const void*)argv,
                            argc);
}

int waitpid(int pid, int* status, int options) {
    return eyn_syscall3_iii(EYN_SYSCALL_WAITPID,
                            pid,
                            (int)(uintptr_t)status,
                            options);
}

int writefile(const char* path, const void* buf, size_t len) {
    if (!path || !buf) return -1;
    if (len > 0x7fffffffU) len = 0x7fffffffU;
    return eyn_syscall3_ppi(EYN_SYSCALL_WRITEFILE, path, buf, (int)len);
}

int mkdir(const char* path, mode_t mode) {
    (void)mode;  /* EYN-OS VFS does not enforce permission bits */
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

#ifdef __chibicc__
void _exit(int code) {
    (void)eyn_syscall1(EYN_SYSCALL_EXIT, code);
    for (;;) {}
}
#else
__attribute__((noreturn)) void _exit(int code) {
    (void)eyn_syscall1(EYN_SYSCALL_EXIT, code);
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
#endif

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

/*
 * lseek() -- reposition an open file descriptor's read offset.
 *
 * Wraps SYSCALL_LSEEK (110).  whence values match POSIX:
 *   SEEK_SET (0): offset from start of file
 *   SEEK_CUR (1): offset from current position
 *   SEEK_END (2): offset from end of file
 *
 * Returns the new offset on success, or -1 on error.
 */
long lseek(int fd, long offset, int whence) {
    return (long)eyn_syscall3_iii(
        EYN_SYSCALL_LSEEK,
        fd,
        (int)offset,
        whence
    );
}

/*
 * access() -- check accessibility of a file path.
 *
 * EYN-OS has no permission model; any path that exists is considered
 * accessible.  We attempt to open the file read-only; if it succeeds the
 * path is accessible (return 0), otherwise it is not (return -1).
 * The mode argument (F_OK / R_OK / X_OK) is accepted but ignored since
 * all checks reduce to "does this path exist".
 */
int access(const char* path, int mode) {
    (void)mode;
    if (!path) return -1;
    int fd = eyn_syscall1(EYN_SYSCALL_OPEN, (int)(uintptr_t)path);
    if (fd < 0) return -1;
    eyn_syscall1(EYN_SYSCALL_CLOSE, fd);
    return 0;
}
