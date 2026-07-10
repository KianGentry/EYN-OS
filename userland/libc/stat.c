#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

/*
 * fstat -- query metadata for an open file descriptor.
 *
 * EYN-OS VFS does not expose extended metadata (ownership, timestamps) to
 * ring-3 programs.  We fill in st_size by seeking to the end and back, and
 * report st_mode as S_IFREG.  This is sufficient for DOOM's M_ReadFile(),
 * which only needs st_size to determine allocation length.
 */
int fstat(int fd, struct stat* st) {
    if (!st) { errno = EINVAL; return -1; }

    /* Determine current position, seek to end for size, seek back. */
    long cur = lseek(fd, 0, SEEK_CUR);
    if (cur < 0) { errno = EBADF; return -1; }

    long end = lseek(fd, 0, SEEK_END);
    if (end < 0) { errno = EBADF; return -1; }

    lseek(fd, cur, SEEK_SET);   /* restore position */

    st->st_mode  = S_IFREG;
    st->st_size  = end;
    st->st_mtime = 0;
    return 0;
}

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
