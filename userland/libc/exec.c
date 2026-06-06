#include <unistd.h>
#include <eynos_syscall.h>
#include <string.h>

typedef struct {
    const char* path;
    const char* const* argv;
    int32_t argc;
    const char* const* envp;
} syscall_exec_req_t;

int execv(const char* path, char* const argv[]) {
    return execve(path, argv, NULL);
}

int execve(const char* path, char* const argv[], char* const envp[]) {
    if (!path) return -1;

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
