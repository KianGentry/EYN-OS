#include <unistd.h>
#include <eynos_syscall.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char* path;
    const char* const* argv;
    int32_t argc;
    const char* const* envp;
} syscall_exec_req_t;

int __eyn_vfork_exec_transition(const char* path, char* const argv[], char* const envp[]);

int execv(const char* path, char* const argv[]) {
    return execve(path, argv, NULL);
}

int execve(const char* path, char* const argv[], char* const envp[]) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }

    int compat_rc = __eyn_vfork_exec_transition(path, argv, envp);
    if (compat_rc != -2) return compat_rc;

    syscall_exec_req_t req;
    memset(&req, 0, sizeof(req));
    req.path = path;
    req.argv = (const char* const*)argv;

    int argc = 0;
    if (argv) {
        while (argv[argc]) argc++;
    }
    req.argc = argc;
    req.envp = (const char* const*)envp;

    return eyn_syscall3_pii(EYN_SYSCALL_EXECVE, &req, 0, 0);
}

static int execvpe_impl(const char* file, char* const argv[], char* const envp[]) {
    char* const* actual_envp = envp ? envp : environ;

    if (!file || !file[0]) {
        errno = ENOENT;
        return -1;
    }

    if (strchr(file, '/')) {
        int rc = execve(file, argv, actual_envp);
        if (rc < 0 && errno == 0) errno = ENOENT;
        return rc;
    }

    const char* path = getenv("PATH");
    if (!path || !path[0]) path = "/binaries:/bin:/usr/bin";

    size_t file_len = strlen(file);
    const char* seg = path;
    for (;;) {
        const char* end = seg;
        while (*end && *end != ':') end++;

        char candidate[256];
        size_t dir_len = (size_t)(end - seg);
        if (dir_len == 0) {
            dir_len = 1;
            candidate[0] = '.';
        } else if (dir_len >= sizeof(candidate)) {
            goto next_seg;
        } else {
            memcpy(candidate, seg, dir_len);
        }

        if (dir_len + 1 + file_len + 1 <= sizeof(candidate)) {
            candidate[dir_len] = '/';
            memcpy(candidate + dir_len + 1, file, file_len + 1);
            execve(candidate, argv, actual_envp);
        }

next_seg:
        if (*end == '\0') break;
        seg = end + 1;
    }

    errno = ENOENT;
    return -1;
}

int execvp(const char* file, char* const argv[]) {
    return execvpe_impl(file, argv, NULL);
}

int execlp(const char* file, const char* arg, ...) {
    va_list ap;
    int argc = 0;
    const char* cur = arg;

    va_start(ap, arg);
    while (cur) {
        argc++;
        cur = va_arg(ap, const char*);
    }
    va_end(ap);

    char** argv = (char**)malloc((size_t)(argc + 1) * sizeof(char*));
    if (!argv) {
        errno = ENOMEM;
        return -1;
    }

    va_start(ap, arg);
    cur = arg;
    for (int i = 0; i < argc; ++i) {
        argv[i] = (char*)cur;
        cur = va_arg(ap, const char*);
    }
    va_end(ap);
    argv[argc] = NULL;

    int rc = execvp(file, argv);
    free(argv);
    return rc;
}
