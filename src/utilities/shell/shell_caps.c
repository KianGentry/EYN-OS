#include <utilities/shell/shell_caps.h>
#include <misc/printf.h>

uint32 shell_get_capabilities(void) {
#if defined(__i386__)
    return (uint32)(SHELL_CAP_PCI | SHELL_CAP_E1000 | SHELL_CAP_NETSTACK |
                    SHELL_CAP_RING3 | SHELL_CAP_PAGING | SHELL_CAP_ATA |
                    SHELL_CAP_GUI | SHELL_CAP_VFS | SHELL_CAP_RAMDISK |
                    SHELL_CAP_MEMDIAG | SHELL_CAP_ISR_DIAG | SHELL_CAP_SERIAL |
                    SHELL_CAP_PANIC);
#elif defined(__aarch64__)
    return (uint32)(SHELL_CAP_GUI | SHELL_CAP_VFS | SHELL_CAP_ATA);
#else
    return 0u;
#endif
}

int shell_command_is_available(const shell_command_info_t* cmd) {
    if (!cmd) return 0;
    if (cmd->required_caps == 0) return 1;
    return ((shell_get_capabilities() & cmd->required_caps) == cmd->required_caps) ? 1 : 0;
}

void shell_unavailable_cmd(string arg) {
    (void)arg;
    const char* name = "(unknown)";
    if (arg) {
        int i = 0;
        while (arg[i] == ' ' || arg[i] == '\t' || arg[i] == '\r' || arg[i] == '\n') i++;
        name = &arg[i];
    }
    char cmd[64];
    int j = 0;
    while (name && name[j] && name[j] != ' ' && name[j] != '\t' && name[j] != '\r' && name[j] != '\n' && j < (int)sizeof(cmd) - 1) {
        cmd[j] = name[j];
        j++;
    }
    cmd[j] = '\0';
    if (!cmd[0]) {
        printf("Command unavailable on this build.\n");
        return;
    }
    printf("Command '%s' is not available on this build.\n", cmd);
}
