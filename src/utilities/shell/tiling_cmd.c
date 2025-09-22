#include <shell.h>
#include <types.h>
#include <tile_manager.h>

void tiling_cmd(string arg) {
    (void)arg;
    start_tiling_manager();
}

// Register command
#include <shell_command_info.h>
REGISTER_SHELL_COMMAND(tiling_cmd_info, "tiling", tiling_cmd, CMD_STREAMING, "Launch the tiling front-end manager.", "tiling");
