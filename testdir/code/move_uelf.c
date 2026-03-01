#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Move a file from source to destination.", "move file1.txt /backup/file1.txt");

static void usage(void) {
    puts("Usage: move <source> <destination>\n"
         "Example: move file1.txt /backup/file1.txt");
}

static const char* basename_simple(const char* path) {
    if (!path) return "";
    size_t n = strlen(path);
    while (n > 0 && path[n - 1] == '/') n--;
    const char* last = path;
    for (size_t i = 0; i < n; i++) {
        if (path[i] == '/') last = &path[i + 1];
    }
    return last;
}

static int is_dir_path(const char* path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return 0;
    eyn_dirent_t dent;
    int rc = getdents(fd, &dent, sizeof(dent));
    close(fd);
    return rc >= 0;
}

static int get_file_size_and_rewind(int fd, int* out_size) {
    if (!out_size) return -1;

    eyn_cap_t cap;
    if (eyn_sys_cap_mint_fd(fd, EYN_CAP_R_SEEK, &cap) != 0) return -1;

    // whence: 2 == SEEK_END
    int size = eyn_sys_cap_fd_seek(&cap, 0, 2);
    if (size < 0) return -1;

    // whence: 0 == SEEK_SET
    if (eyn_sys_cap_fd_seek(&cap, 0, 0) < 0) return -1;

    *out_size = size;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3 || !argv[1] || !argv[2] || !argv[1][0] || !argv[2][0]) {
        usage();
        return 1;
    }
    if (strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    const char* src = argv[1];
    const char* dst = argv[2];

    int src_fd = open(src, O_RDONLY, 0);
    if (src_fd < 0) {
        printf("move: source file not found: %s\n", src);
        return 1;
    }

    // Reject directories as sources.
    {
        eyn_dirent_t dent;
        int grc = getdents(src_fd, &dent, sizeof(dent));
        if (grc >= 0) {
            close(src_fd);
            printf("move: source is a directory: %s\n", src);
            return 1;
        }
    }

    char dst_path[128];
    if (is_dir_path(dst)) {
        const char* base = basename_simple(src);
        if (!base[0]) {
            close(src_fd);
            puts("move: invalid source filename");
            return 1;
        }

        size_t dlen = strlen(dst);
        int need_slash = (dlen > 0 && dst[dlen - 1] != '/');
        size_t blen = strlen(base);

        size_t outlen = dlen + (need_slash ? 1 : 0) + blen;
        if (outlen >= sizeof(dst_path)) {
            close(src_fd);
            puts("move: destination path too long");
            return 1;
        }

        strcpy(dst_path, dst);
        if (need_slash) strcat(dst_path, "/");
        strcat(dst_path, base);
        dst = dst_path;
    }

    int size = 0;
    int have_size = (get_file_size_and_rewind(src_fd, &size) == 0);

    int bytes_written = 0;
    if (have_size && size == 0) {
        const char dummy = 0;
        int w = writefile(dst, &dummy, 0);
        close(src_fd);
        if (w < 0) {
            puts("move: failed to create destination file");
            return 1;
        }
        bytes_written = 0;
    } else {
        int stream = eynfs_stream_begin(dst);
        if (stream < 0) {
            close(src_fd);
            puts("move: streaming write not supported for destination");
            return 1;
        }

        int ok = 1;
        int total = 0;
        uint8_t buf[4096];
        for (;;) {
            ssize_t n = read(src_fd, buf, sizeof(buf));
            if (n < 0) {
                ok = 0;
                puts("move: failed to read source file");
                break;
            }
            if (n == 0) break;
            ssize_t w = eynfs_stream_write(stream, buf, (size_t)n);
            if (w != n) {
                ok = 0;
                puts("move: failed to write destination file");
                break;
            }
            total += (int)n;
        }

        close(src_fd);
        if (eynfs_stream_end(stream) != 0) {
            ok = 0;
            puts("move: failed to finalize destination file");
        }

        if (!ok) {
            (void)unlink(dst);
            return 1;
        }

        bytes_written = total;
    }

    if (unlink(src) != 0) {
        printf("move: warning: file copied but failed to delete source: %s\n", src);
        return 1;
    }

    printf("File moved: %s -> %s (%d bytes)\n", src, dst, bytes_written);
    return 0;
}
