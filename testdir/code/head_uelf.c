#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Print the first lines of a file.", "head -n 10 /test.txt");

static int parse_u32(const char* s, unsigned int* out) {
    if (!s || !s[0] || !out) return -1;
    char* end = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (!end || *end != '\0') return -1;
    *out = (unsigned int)v;
    return 0;
}

static void usage(void) {
    puts("Usage: head [-n lines] <file>\n"
         "Example: head -n 10 /test.txt");
}

int main(int argc, char** argv) {
    unsigned int lines = 10;
    const char* path = 0;

    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    if (argc >= 4 && argv[1] && strcmp(argv[1], "-n") == 0) {
        if (parse_u32(argv[2], &lines) != 0 || lines == 0) {
            puts("head: invalid line count");
            return 1;
        }
        path = argv[3];
    } else if (argc >= 2) {
        path = argv[1];
    }

    if (!path || !path[0]) {
        usage();
        return 1;
    }

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("head: failed to open: %s\n", path);
        return 1;
    }

    char buf[256];
    unsigned int seen = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            puts("head: read error");
            close(fd);
            return 1;
        }
        if (n == 0) break;

        for (ssize_t i = 0; i < n; ++i) {
            if (seen >= lines) {
                close(fd);
                return 0;
            }
            putchar(buf[i]);
            if (buf[i] == '\n') seen++;
        }
    }

    close(fd);
    return 0;
}
