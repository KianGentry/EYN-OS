#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Sort strings alphabetically.", "sort zebra apple banana");

static void usage(void) {
    puts("Usage: sort <string1> <string2> ...\n"
         "Example: sort zebra apple banana");
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }
    if (argc < 2) {
        usage();
        return 1;
    }

    for (int i = 1; i < argc - 1; ++i) {
        for (int j = i + 1; j < argc; ++j) {
            if (strcmp(argv[i], argv[j]) > 0) {
                char* t = argv[i];
                argv[i] = argv[j];
                argv[j] = t;
            }
        }
    }

    for (int i = 1; i < argc; ++i) {
        printf("%s\n", argv[i]);
    }
    return 0;
}
