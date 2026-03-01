#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>

// Optional help metadata consumed by the kernel's `help` command.
// This is stored in an ELF section named `.eynos.cmdmeta`.
EYN_CMDMETA_V1("Delete a file or directory.", "delete test.txt");

static int ends_with_slash(const char* s) {
    if (!s) return 0;
    size_t n = 0;
    while (s[n]) n++;
    return (n > 0 && s[n - 1] == '/');
}

static void usage(void) {
    puts("Usage: delete <path>\n"
         "Examples:\n"
         "  delete test/\n"
         "  delete test.txt");
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
        int rc = rmdir(path);
        if (rc != 0) {
            printf("delete: failed to delete directory: %s\n", path);
            return 1;
        }
        return 0;
    }

    int rc = unlink(path);
    if (rc != 0) {
        printf("delete: failed to delete file: %s\n", path);
        return 1;
    }
    return 0;
}
