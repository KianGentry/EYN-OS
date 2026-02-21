#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(void) {
    char path[128];
    puts("cat: enter path:");
    int n = (int)read(0, path, sizeof(path));
    if (n <= 0) {
        puts("cat: no input");
        return 1;
    }

    // `readStr()` returns a newline-terminated line; strip it.
    if (n > 0 && (path[n - 1] == '\n' || path[n - 1] == '\r')) {
        path[n - 1] = '\0';
        n--;
    }
    while (n > 0 && (path[n - 1] == '\n' || path[n - 1] == '\r')) {
        path[n - 1] = '\0';
        n--;
    }
    if (path[0] == '\0') {
        puts("cat: empty path");
        return 1;
    }

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("cat: failed to open: %s\n", path);
        return 1;
    }

    char buf[256];
    for (;;) {
        int r = (int)read(fd, buf, sizeof(buf));
        if (r < 0) {
            puts("cat: read error");
            break;
        }
        if (r == 0) break;
        (void)write(1, buf, (size_t)r);
    }

    (void)close(fd);
    return 0;
}
