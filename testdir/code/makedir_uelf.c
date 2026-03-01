#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Create a new directory.", "makedir myfolder");

static void usage(void) {
    puts("Usage: makedir <directory>");
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        usage();
        return 1;
    }

    if (mkdir(argv[1]) != 0) {
        printf("makedir: failed to create directory: %s\n", argv[1]);
        return 1;
    }

    return 0;
}
