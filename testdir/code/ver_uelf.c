#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Show system version information.", "ver");

static void usage(void) {
    puts("Usage: ver");
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    puts("EYN-OS Release 16");
    return 0;
}
