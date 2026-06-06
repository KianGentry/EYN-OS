#ifndef SPAWN_H
#define SPAWN_H

#include <sys/types.h>

/* Minimal prototype for posix_spawn; attr and file_actions are not fully supported yet. */
int posix_spawn(pid_t *pid, const char *path, const void *file_actions,
               const void *attrp, char *const argv[], char *const envp[]);

#endif
