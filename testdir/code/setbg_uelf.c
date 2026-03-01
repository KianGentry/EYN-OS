#include <stdio.h>
#include <string.h>
#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Set background image for focused tile.", "setbg /images/eynos.rei");

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: setbg <rei_file>");
        return 0;
    }

    if (argc < 2 || !argv[1] || !argv[1][0]) {
        puts("setbg: missing image path");
        puts("Usage: setbg <rei_file>");
        return 1;
    }

    if (eyn_sys_setbg_path(argv[1]) != 0) {
        printf("setbg: failed to set background from: %s\n", argv[1]);
        return 1;
    }

    return 0;
}
