#ifndef SHELL_CAPS_H
#define SHELL_CAPS_H

#include <misc/types.h>
#include <utilities/shell/shell_command_info.h>

typedef enum {
    SHELL_CAP_PCI = 1u << 0,
    SHELL_CAP_E1000 = 1u << 1,
    SHELL_CAP_NETSTACK = 1u << 2,
    SHELL_CAP_RING3 = 1u << 3,
    SHELL_CAP_PAGING = 1u << 4,
    SHELL_CAP_ATA = 1u << 5,
    SHELL_CAP_GUI = 1u << 6,
    SHELL_CAP_VFS = 1u << 7,
    SHELL_CAP_RAMDISK = 1u << 8,
    SHELL_CAP_MEMDIAG = 1u << 9,
    SHELL_CAP_ISR_DIAG = 1u << 10,
    SHELL_CAP_SERIAL = 1u << 11,
    SHELL_CAP_PANIC = 1u << 12
} shell_capability_t;

uint32 shell_get_capabilities(void);
int shell_command_is_available(const shell_command_info_t* cmd);
void shell_unavailable_cmd(string arg);

#endif // SHELL_CAPS_H
