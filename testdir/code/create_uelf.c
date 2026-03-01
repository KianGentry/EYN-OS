#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>

// Optional help metadata consumed by the kernel's `help` command.
// This is stored in an ELF section named `.eynos.cmdmeta`.
EYN_CMDMETA_V1("Create a file or directory.", "create test/");

static int ends_with_slash(const char* s) {
    if (!s) return 0;
    size_t n = strlen(s);
    return (n > 0 && s[n - 1] == '/');
}

static void usage(void) {
    puts("Usage: create <path>\n"
         "Examples:\n"
         "  create test/\n"
         "  create test.txt");
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        usage();
        return 1;
    }
    if (strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    const char* path = argv[1];

    if (ends_with_slash(path)) {
        int rc = mkdir(path);
        if (rc != 0) {
            printf("create: failed to create directory: %s\n", path);
            return 1;
        }
        return 0;
    }

    // Create an empty file.
    const char dummy = 0;
    int written = writefile(path, &dummy, 0);
    if (written < 0) {
        printf("create: failed to create file: %s\n", path);
        return 1;
    }
    return 0;
}
