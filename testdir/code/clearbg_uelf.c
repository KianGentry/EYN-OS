#include <stdio.h>
#include <string.h>
#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Clear background image for focused tile.", "clearbg");

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: clearbg");
        return 0;
    }

    if (eyn_sys_clearbg_focused() != 0) {
        puts("clearbg: failed to clear focused background");
        return 1;
    }

    return 0;
}
