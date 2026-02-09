#ifndef CONTEXT_H
#define CONTEXT_H

#include <types.h>

typedef struct sched_work sched_work_t;

typedef struct command_context_t {
    uint32 caps;
    sched_work_t* wo;
    uint32 det_seq;
    uint8 drive;
    uint8 _pad[3];
} command_context_t;

// Capability map bits (coarse-grained command context permissions).
#define CAP_WRITE_CONSOLE (1u << 0)
#define CAP_READ_FS       (1u << 1)
#define CAP_WRITE_FS      (1u << 2)
#define CAP_ALLOC_MEMORY  (1u << 3)
#define CAP_DEV_SERIAL    (1u << 4)
#define CAP_DEV_DISK      (1u << 5)

#define CAP_ALL (CAP_WRITE_CONSOLE | CAP_READ_FS | CAP_WRITE_FS | CAP_ALLOC_MEMORY | CAP_DEV_SERIAL | CAP_DEV_DISK)

extern command_context_t* current_command_context;

void command_context_set(command_context_t* ctx);
void command_context_clear(void);
command_context_t* command_context_get(void);

static inline int cap_check(uint32 caps, uint32 cap) {
    return ((caps & cap) == cap) ? 1 : 0;
}

#endif // CONTEXT_H
