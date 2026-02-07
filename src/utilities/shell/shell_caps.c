#include <utilities/shell/shell_caps.h>

uint32 shell_get_capabilities(void) {
#if defined(__i386__)
    return (uint32)(SHELL_CAP_PCI | SHELL_CAP_E1000 | SHELL_CAP_NETSTACK |
                    SHELL_CAP_RING3 | SHELL_CAP_PAGING | SHELL_CAP_ATA |
                    SHELL_CAP_GUI | SHELL_CAP_VFS);
#elif defined(__aarch64__)
    return (uint32)(SHELL_CAP_GUI | SHELL_CAP_VFS);
#else
    return 0u;
#endif
}

int shell_command_is_available(const shell_command_info_t* cmd) {
    if (!cmd) return 0;
    if (cmd->required_caps == 0) return 1;
    return ((shell_get_capabilities() & cmd->required_caps) == cmd->required_caps) ? 1 : 0;
}
