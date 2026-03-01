#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Run a native program, .uelf, or script.", "run <program> [args...]");

static void usage(void) {
    puts("Usage: run <program.eyn|program.bin|program.flat|program.uelf|script.shell> [args...]");
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        usage();
        return 1;
    }

    char raw[192];
    raw[0] = '\0';

    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) continue;
        if (raw[0]) {
            if (strlen(raw) + 1 >= sizeof(raw)) break;
            strcat(raw, " ");
        }
        size_t left = sizeof(raw) - strlen(raw) - 1;
        if (left == 0) break;
        strncat(raw, argv[i], left);
    }

    if (!raw[0]) {
        usage();
        return 1;
    }

    if (eyn_sys_run(raw) != 0) {
        puts("run: failed");
        return 1;
    }

    return 0;
}
