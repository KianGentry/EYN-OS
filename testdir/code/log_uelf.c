#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Enable or disable shell logging.", "log on");

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1]) {
        puts("Usage: log on|off");
        return 1;
    }
    if (strcmp(argv[1], "-h") == 0) {
        puts("Usage: log on|off");
        return 0;
    }
    if (strcmp(argv[1], "on") == 0) {
        int state = eyn_sys_shell_log_set(1);
        if (state < 0) {
            puts("log: failed to enable logging");
            return 1;
        }
        puts("logging enabled");
        return 0;
    }
    if (strcmp(argv[1], "off") == 0) {
        int state = eyn_sys_shell_log_set(0);
        if (state < 0) {
            puts("log: failed to disable logging");
            return 1;
        }
        puts("logging disabled");
        return 0;
    }
    puts("Usage: log on|off");
    return 1;
}
