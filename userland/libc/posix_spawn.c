#include <spawn.h>
#include <eynos_syscall.h>
#include <string.h>

int posix_spawn(pid_t *pid, const char *path, const void *file_actions,
               const void *attrp, char *const argv[], char *const envp[]) {
    (void)file_actions;
    (void)attrp;
    (void)envp;

    if (!path) return -1;

    eyn_spawn_ex_req_t req;
    memset(&req, 0, sizeof(req));
    req.path = path;
    req.argv = (const char* const*)argv;

    int argc = 0;
    if (argv) {
        while (argv[argc]) argc++;
    }
    req.argc = argc;
    req.stdin_fd = 0;
    req.stdout_fd = 1;
    req.stderr_fd = 2;
    req.inherit_mode = 1;

    int rc = eyn_syscall3_pii(EYN_SYSCALL_SPAWN_EX, &req, 0, 0);
    if (rc < 0) return -1;
    if (pid) *pid = (pid_t)rc;
    return 0;
}
