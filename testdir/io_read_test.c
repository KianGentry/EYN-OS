// File I/O smoke test.
// Exercises: open/read/close syscalls via libc, basic buffer handling.

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main(void) {
    int fd = open("test.txt", O_RDONLY, 0);
    if (fd < 0) {
        printf("io_read_test: open failed (%d)\n", fd);
        return 1;
    }

    char buf[16];
    int n = (int)read(fd, buf, sizeof(buf));
    close(fd);

    if (n != 5) {
        printf("io_read_test: read n=%d (expected 5)\n", n);
        return 1;
    }

    if (!(buf[0] == 'b' && buf[1] == 'a' && buf[2] == 'l' && buf[3] == 'l' && buf[4] == 's')) {
        printf("io_read_test: contents mismatch\n");
        printf("io_read_test: got '%c%c%c%c%c'\n", buf[0], buf[1], buf[2], buf[3], buf[4]);
        return 1;
    }

    int sum = 0;
    for (int i = 0; i < 5; i++) sum += (unsigned char)buf[i];

    printf("io_read_test: ok '%c%c%c%c%c' sum=%d\n", buf[0], buf[1], buf[2], buf[3], buf[4], sum);
    return 0;
}
