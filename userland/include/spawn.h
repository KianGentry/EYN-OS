#ifndef SPAWN_H
#define SPAWN_H

#include <sys/types.h>

/* Minimal posix_spawn file actions support.
 * We only support dup2 actions that remap fds 0/1/2.
 */
typedef struct {
    int stdio_map[3]; /* -1 = not set, otherwise source fd for target 0/1/2 */
} posix_spawn_file_actions_t;

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions, int fd, int newfd);

/* Minimal prototype for posix_spawn; attr is ignored. */
int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *file_actions,
               const void *attrp, char *const argv[], char *const envp[]);

#endif
