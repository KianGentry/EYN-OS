#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Clear terminal output.", "clear");

static void usage(void) {
    puts("Usage: clear");
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    if (eyn_sys_vterm_clear() == 0) return 0;

    // Fallback for environments that do not yet expose vterm clear.
    printf("\x1b[2J\x1b[H");
    return 0;
}
