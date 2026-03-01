#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Print the current working directory.", "pwd");

static void usage(void) {
    puts("Usage: pwd");
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && argv[1][0] && (strcmp(argv[1], "-h") == 0)) {
        usage();
        return 0;
    }

    char cwd[128];
    int n = getcwd(cwd, sizeof(cwd));
    if (n < 0) {
        puts("pwd: failed to query cwd");
        return 1;
    }

    puts(cwd);
    return 0;
}
