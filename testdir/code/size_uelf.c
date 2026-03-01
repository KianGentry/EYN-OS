#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Show the size of a file in bytes.", "size myfile.txt");

static void usage(void) {
    puts("Usage: size <file>\n"
         "Example: size test.txt");
}

static int is_dir_fd(int fd) {
    eyn_dirent_t dent;
    int rc = getdents(fd, &dent, sizeof(dent));
    return rc >= 0;
}

static int get_file_size_via_seek(int fd) {
    eyn_cap_t cap;
    if (eyn_sys_cap_mint_fd(fd, EYN_CAP_R_SEEK, &cap) != 0) {
        return -1;
    }
    // whence: 2 == SEEK_END
    int size = eyn_sys_cap_fd_seek(&cap, 0, 2);
    return size;
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
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("size: file not found: %s\n", path);
        return 1;
    }

    if (is_dir_fd(fd)) {
        close(fd);
        printf("size: not a file: %s\n", path);
        return 1;
    }

    int size = get_file_size_via_seek(fd);
    close(fd);
    if (size < 0) {
        printf("size: failed: %s\n", path);
        return 1;
    }

    printf("%s: %d bytes\n", path, size);
    return 0;
}
