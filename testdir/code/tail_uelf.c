#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Print the last lines of a file.", "tail -n 10 /test.txt");

static int parse_u32(const char* s, unsigned int* out) {
    if (!s || !s[0] || !out) return -1;
    char* end = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (!end || *end != '\0') return -1;
    *out = (unsigned int)v;
    return 0;
}

static void usage(void) {
    puts("Usage: tail [-n lines] <file>\n"
         "Example: tail -n 10 /test.txt");
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
            puts("tail: invalid line count");
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
        printf("tail: failed to open: %s\n", path);
        return 1;
    }

    int cap = 4096;
    int len = 0;
    char* data = (char*)malloc((size_t)cap);
    if (!data) {
        close(fd);
        puts("tail: out of memory");
        return 1;
    }

    char chunk[256];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            free(data);
            close(fd);
            puts("tail: read error");
            return 1;
        }
        if (n == 0) break;

        if (len + (int)n > cap) {
            int new_cap = cap;
            while (len + (int)n > new_cap) new_cap *= 2;
            char* bigger = (char*)realloc(data, (size_t)new_cap);
            if (!bigger) {
                free(data);
                close(fd);
                puts("tail: out of memory");
                return 1;
            }
            data = bigger;
            cap = new_cap;
        }

        memcpy(data + len, chunk, (size_t)n);
        len += (int)n;
    }
    close(fd);

    int breaks = 0;
    int start = len;
    for (int i = len - 1; i >= 0; --i) {
        if (data[i] == '\n') {
            breaks++;
            if ((unsigned int)breaks > lines) {
                start = i + 1;
                break;
            }
        }
    }

    for (int i = start; i < len; ++i) putchar(data[i]);
    free(data);
    return 0;
}
