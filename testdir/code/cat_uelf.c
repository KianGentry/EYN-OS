#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include <eynos_cmdmeta.h>

// Optional help metadata consumed by the kernel's `help` command.
// This is stored in an ELF section named `.eynos.cmdmeta`.
EYN_CMDMETA_V1("Print a file to stdout.", "read /test.txt");

static void usage(void) {
    puts("Usage: read <path>\nExample: read /test.txt");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 1;
    }
    if (argv[1][0] == '-' && argv[1][1] == 'h' && argv[1][2] == '\0') {
        usage();
        return 0;
    }
    if (argv[1][0] == '\0') {
        usage();
        return 1;
    }
    const char* path = argv[1];

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("read: failed to open: %s\n", path);
        return 1;
    }

    char buf[256];
    for (;;) {
        int r = (int)read(fd, buf, sizeof(buf));
        if (r < 0) {
            puts("read: read error");
            break;
        }
        if (r == 0) break;
        (void)write(1, buf, (size_t)r);
    }

    (void)close(fd);
    return 0;
}
