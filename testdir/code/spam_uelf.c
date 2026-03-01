#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Spam 'EYN-OS' to stdout 100 times.", "spam");

static void usage(void) {
    puts("Usage: spam");
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    for (int i = 0; i < 100; ++i) {
        puts("EYN-OS");
    }
    return 0;
}
