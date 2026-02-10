#include <shell.h>
#include <types.h>
#include <tile_manager.h>
#include <context.h>
#include <misc/sched.h>

static int tiling_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

void tiling_cmd(string arg) {
    (void)arg;
    if (!tiling_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) return;
    start_tiling_manager();
}

// Register command
#include <shell_command_info.h>
REGISTER_SHELL_COMMAND(tiling_cmd_info, "tiling", tiling_cmd, CMD_STREAMING, "Launch the tiling front-end manager.", "tiling");
