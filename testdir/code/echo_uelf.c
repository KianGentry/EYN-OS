#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Print arguments to stdout.", "echo hello world");

static void usage(void) {
    puts("Usage: echo [-n] [text...]\n"
         "Example: echo hello world");
}

int main(int argc, char** argv) {
    int newline = 1;
    int i = 1;

    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    if (argc >= 2 && argv[1] && strcmp(argv[1], "-n") == 0) {
        newline = 0;
        i = 2;
    }

    for (; i < argc; ++i) {
        if (!argv[i]) continue;
        if (i != (newline ? 1 : 2)) putchar(' ');
        fputs(argv[i], stdout);
    }

    if (newline) putchar('\n');
    return 0;
}
