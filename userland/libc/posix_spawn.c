#include <spawn.h>
#include <eynos_syscall.h>
#include <string.h>

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions) {
    if (!actions) return -1;
    actions->stdio_map[0] = -1;
    actions->stdio_map[1] = -1;
    actions->stdio_map[2] = -1;
    return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions) {
    (void)actions;
    return 0;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions, int fd, int newfd) {
    if (!actions) return -1;
    if (newfd < 0 || newfd > 2) {
        /* only support remapping standard fds for now */
        return -1;
    }
    actions->stdio_map[newfd] = fd;
    return 0;
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *file_actions,
               const void *attrp, char *const argv[], char *const envp[]) {
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

    /* Default stdio mapping */
    req.stdin_fd = 0;
    req.stdout_fd = 1;
    req.stderr_fd = 2;
    req.inherit_mode = 1;

    if (file_actions) {
        /* Honor dup2 actions that remap 0/1/2 */
        for (int i = 0; i < 3; ++i) {
            if (file_actions->stdio_map[i] >= 0) {
                if (i == 0) req.stdin_fd = file_actions->stdio_map[i];
                if (i == 1) req.stdout_fd = file_actions->stdio_map[i];
                if (i == 2) req.stderr_fd = file_actions->stdio_map[i];
            }
        }
        /* If user explicitly sets stdio remaps, don't rely on fd inheritance */
        req.inherit_mode = 0;
    }

    int rc = eyn_syscall3_pii(EYN_SYSCALL_SPAWN_EX, &req, 0, 0);
    if (rc < 0) return -1;
    if (pid) *pid = (pid_t)rc;
    return 0;
}
