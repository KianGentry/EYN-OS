#include <context.h>

/*
 * SECURITY-INVARIANT: current_command_context must never point to a stack frame
 * that can be abandoned by ring3 transitions or UI re-entry.
 *
 * Why: Several subsystems (disk/FS, alloc accounting, deterministic tracing)
 * dereference current_command_context during interrupts/page faults. When a
 * ring3 task exits, the kernel may resume execution via a different control
 * path (e.g. UI restart) without unwinding the original caller's stack.
 *
 * Invariant: The active command context is stored in kernel-owned memory with
 * a bounded depth stack; pointers remain valid until popped/cleared.
 * Breakage if changed: Allowing pointers to stack-allocated contexts risks
 * use-after-free and kernel-mode page-fault storms during swap/disk I/O.
 */

#define COMMAND_CONTEXT_STACK_MAX 32

static command_context_t g_ctx_stack[COMMAND_CONTEXT_STACK_MAX];
static int g_ctx_top = -1;

command_context_t* current_command_context = 0;

int command_context_push(const command_context_t* ctx) {
    if (!ctx) {
        return 0;
    }
    if (g_ctx_top + 1 >= COMMAND_CONTEXT_STACK_MAX) {
        /* Fail closed: keep previous context rather than corrupting it. */
        return 0;
    }
    g_ctx_top++;
    g_ctx_stack[g_ctx_top] = *ctx;
    current_command_context = &g_ctx_stack[g_ctx_top];
    return 1;
}

void command_context_pop(void) {
    if (g_ctx_top >= 0) {
        g_ctx_top--;
    }
    if (g_ctx_top >= 0) {
        current_command_context = &g_ctx_stack[g_ctx_top];
    } else {
        current_command_context = 0;
    }
}

void command_context_set(command_context_t* ctx) {
    if (!ctx) {
        command_context_clear();
        return;
    }
    (void)command_context_push(ctx);
}

void command_context_clear(void) {
    g_ctx_top = -1;
    current_command_context = 0;
}

command_context_t* command_context_get(void) {
    return current_command_context;
}
