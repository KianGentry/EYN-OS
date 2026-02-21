#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

int stat(const char* path, struct stat* st) {
    if (!path || !st) { errno = EINVAL; return -1; }

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) { errno = ENOENT; return -1; }

    // Determine if directory by attempting getdents.
    eyn_dirent_t dent;
    int grc = getdents(fd, &dent, sizeof(dent));
    if (grc >= 0) {
        st->st_mode = S_IFDIR;
        st->st_size = 0;
        st->st_mtime = 0;
        close(fd);
        return 0;
    }

    // File: estimate size by reading to EOF.
    long total = 0;
    for (;;) {
        char buf[256];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) { close(fd); errno = EINVAL; return -1; }
        if (n == 0) break;
        total += n;
    }

    close(fd);
    st->st_mode = S_IFREG;
    st->st_size = total;
    st->st_mtime = 0;
    return 0;
}
