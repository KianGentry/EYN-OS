#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Trigger an assertion failure (ASSERT).", "assertfail yes");

static void usage(void) {
    puts("Usage: assertfail yes");
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    if (argc < 2 || !argv[1] || strcmp(argv[1], "yes") != 0) {
        usage();
        return 1;
    }

    puts("assertfail: triggering deliberate crash");
    *(volatile int*)0 = 1;
    return 1;
}
