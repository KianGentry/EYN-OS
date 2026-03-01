#include <stdio.h>
#include <string.h>
#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Launch the tiling manager.", "tiling");

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: tiling");
        return 0;
    }

    if (eyn_sys_tiling_start() != 0) {
        puts("tiling: failed to start tiling mode");
        return 1;
    }

    return 0;
}
