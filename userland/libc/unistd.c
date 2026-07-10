#include <unistd.h>
#include <eynos_syscall.h>

#include <errno.h>
#include <setjmp.h>
#include <stdint.h>

typedef struct {
    int active;
    int child_active;
    int child_stdin_fd;
    int child_stdout_fd;
    int child_stderr_fd;
    int child_inherit_mode;
    int synthetic_wait_active;
    int synthetic_status;
    pid_t synthetic_pid;
    pid_t resume_pid;
    jmp_buf parent_env;
} eyn_vfork_compat_t;

static eyn_vfork_compat_t g_vfork_compat;
static pid_t g_vfork_next_synthetic_pid = 0x70000000;
static int g_shadow_stdin_fd = 0;
static int g_shadow_stdout_fd = 1;
static int g_shadow_stderr_fd = 2;
static int g_shadow_inherit_mode = 1;

static int eyn_vfork_child_active(void) {
    return g_vfork_compat.active && g_vfork_compat.child_active;
}

int __eyn_vfork_exec_transition(const char* path, char* const argv[], char* const envp[]) {
    (void)envp;

    if (!eyn_vfork_child_active()) return -2;
    if (!path) {
        errno = EINVAL;
        return -1;
    }

    int argc = 0;
    if (argv) {
        while (argv[argc]) argc++;
    }

    eyn_spawn_ex_req_t req;
    req.path = path;
    req.argv = (const char* const*)argv;
    req.argc = argc;
    req.stdin_fd = g_vfork_compat.child_stdin_fd;
    req.stdout_fd = g_vfork_compat.child_stdout_fd;
    req.stderr_fd = g_vfork_compat.child_stderr_fd;
    req.inherit_mode = g_vfork_compat.child_inherit_mode;

    int pid = eyn_syscall3_pii(EYN_SYSCALL_SPAWN_EX, &req, 0, 0);
    if (pid < 0) {
        if (errno == 0) errno = ENOENT;
        return -1;
    }

    g_vfork_compat.resume_pid = pid;
    g_vfork_compat.child_active = 0;
    longjmp(g_vfork_compat.parent_env, 1);
}

void __eyn_vfork_child_exit(int code) {
    if (!eyn_vfork_child_active()) return;

    if (g_vfork_next_synthetic_pid <= 0) g_vfork_next_synthetic_pid = 0x70000000;
    g_vfork_compat.synthetic_pid = g_vfork_next_synthetic_pid++;
    g_vfork_compat.synthetic_status = code;
    g_vfork_compat.synthetic_wait_active = 1;
    g_vfork_compat.resume_pid = g_vfork_compat.synthetic_pid;
    g_vfork_compat.child_active = 0;
    longjmp(g_vfork_compat.parent_env, 1);
}

pid_t vfork(void) {
    if (g_vfork_compat.active) {
        errno = EAGAIN;
        return -1;
    }

    g_vfork_compat.active = 1;
    g_vfork_compat.child_active = 1;
    g_vfork_compat.child_stdin_fd = g_shadow_stdin_fd;
    g_vfork_compat.child_stdout_fd = g_shadow_stdout_fd;
    g_vfork_compat.child_stderr_fd = g_shadow_stderr_fd;
    g_vfork_compat.child_inherit_mode = g_shadow_inherit_mode;

    if (setjmp(g_vfork_compat.parent_env) == 0) {
        return 0;
    }

    g_vfork_compat.active = 0;
    return g_vfork_compat.resume_pid;
}

pid_t fork(void) {
    return vfork();
}

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
    if (eyn_vfork_child_active()) {
        if (fd <= 2) {
            errno = ENOSYS;
            return -1;
        }
        g_vfork_compat.child_inherit_mode = 0;
        return 0;
    }
    return eyn_syscall1(EYN_SYSCALL_CLOSE, fd);
}

int dup(int oldfd) {
    if (eyn_vfork_child_active()) {
        errno = ENOSYS;
        return -1;
    }
    return eyn_syscall1(EYN_SYSCALL_DUP, oldfd);
}

int dup2(int oldfd, int newfd) {
    if (eyn_vfork_child_active()) {
        if (newfd == 0) {
            g_vfork_compat.child_stdin_fd = oldfd;
            return 0;
        }
        if (newfd == 1) {
            g_vfork_compat.child_stdout_fd = oldfd;
            return 1;
        }
        if (newfd == 2) {
            g_vfork_compat.child_stderr_fd = oldfd;
            return 2;
        }
        errno = ENOSYS;
        return -1;
    }
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
    if (eyn_vfork_child_active()) {
        int prev = g_vfork_compat.child_inherit_mode;
        g_vfork_compat.child_inherit_mode = enabled ? 1 : 0;
        return prev;
    }
    g_shadow_inherit_mode = enabled ? 1 : 0;
    return eyn_syscall1(EYN_SYSCALL_FD_SET_INHERIT, enabled ? 1 : 0);
}

int fd_set_stdio(int stdin_fd, int stdout_fd, int stderr_fd) {
    if (eyn_vfork_child_active()) {
        g_vfork_compat.child_stdin_fd = stdin_fd;
        g_vfork_compat.child_stdout_fd = stdout_fd;
        g_vfork_compat.child_stderr_fd = stderr_fd;
        return 0;
    }
    g_shadow_stdin_fd = stdin_fd;
    g_shadow_stdout_fd = stdout_fd;
    g_shadow_stderr_fd = stderr_fd;
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
    if (g_vfork_compat.synthetic_wait_active && (pid == -1 || pid == g_vfork_compat.synthetic_pid)) {
        if (status) *status = g_vfork_compat.synthetic_status;
        g_vfork_compat.synthetic_wait_active = 0;
        return g_vfork_compat.synthetic_pid;
    }
    return eyn_syscall3_iii(EYN_SYSCALL_WAITPID,
                            pid,
                            (int)(uintptr_t)status,
                            options);
}

int wait(int* status) {
    return waitpid(-1, status, 0);
}

int kill(int pid, int sig) {
    return eyn_syscall3_iii(EYN_SYSCALL_KILL, pid, sig, 0);
}

int sigreturn(void) {
    return eyn_syscall0(EYN_SYSCALL_SIGRETURN);
}

/* Install a signal handler for the calling process. */
sighandler_t signal(int sig, sighandler_t handler) {
    (void)eyn_syscall3_iii(EYN_SYSCALL_SIGNAL, sig, (int)(uintptr_t)handler, 0);
    return (sighandler_t)0;
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
    if (eyn_vfork_child_active()) {
        __eyn_vfork_child_exit(code);
    }
    (void)eyn_syscall1(EYN_SYSCALL_EXIT, code);
    for (;;) {}
}
#else
__attribute__((noreturn)) void _exit(int code) {
    if (eyn_vfork_child_active()) {
        __eyn_vfork_child_exit(code);
    }
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

void* mmap(void* addr, size_t length, int prot, int flags, int fd, long offset) {
    uintptr_t ret;
    int off = (int)offset;

    if (length == 0) return (void*)-1;

    __asm__ __volatile__(
        "push %%ebp\n\t"
        "movl %7, %%ebp\n\t"
        "int $0x80\n\t"
        "pop %%ebp\n\t"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_MMAP), "b"(addr), "c"(length), "d"(prot), "S"(flags), "D"(fd), "m"(off)
        : "memory"
    );

    if (ret == (uintptr_t)-1) return (void*)-1;
    return (void*)(uintptr_t)ret;
}

int munmap(void* addr, size_t length) {
    return eyn_syscall3_iii(EYN_SYSCALL_MUNMAP, (int)(uintptr_t)addr, (int)length, 0);
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
