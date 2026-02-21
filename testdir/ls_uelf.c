#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

int main(void) {
    char path[128];
    puts("ls: enter path (empty = /):");
    int n = (int)read(0, path, sizeof(path));
    if (n < 0) {
        puts("ls: input error");
        return 1;
    }
    if (path[0] == '\0') {
        path[0] = '/';
        path[1] = '\0';
    }

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("ls: failed to open: %s\n", path);
        return 1;
    }

    eyn_dirent_t ents[16];
    for (;;) {
        int rc = getdents(fd, ents, sizeof(ents));
        if (rc < 0) {
            puts("ls: getdents error");
            break;
        }
        if (rc == 0) break;
        int count = rc / (int)sizeof(eyn_dirent_t);
        for (int i = 0; i < count; ++i) {
            if (ents[i].name[0] == '\0') continue;
            if (ents[i].is_dir) {
                printf("%s/\n", ents[i].name);
            } else {
                printf("%s\n", ents[i].name);
            }
        }
    }

    (void)close(fd);
    return 0;
}
