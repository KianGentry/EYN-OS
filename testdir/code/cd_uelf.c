#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Change the current directory.", "cd <directory>");

static void usage(void) {
    puts("Usage: cd <directory>");
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        usage();
        return 1;
    }

    if (chdir(argv[1]) != 0) {
        printf("cd: directory not found: %s\n", argv[1]);
        return 1;
    }

    return 0;
}
