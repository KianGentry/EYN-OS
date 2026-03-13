#include <stddef.h>
#include <sys/types.h>  /* mode_t, off_t */

typedef long ssize_t;

// chibicc doesn't implement GNU __attribute__ yet.
#ifdef __chibicc__
#define EYN_ATTR_NORETURN
#else
#define EYN_ATTR_NORETURN __attribute__((noreturn))
#endif

// EYN-OS supports fd=0 (stdin), fd=1 (stdout) today.
ssize_t write(int fd, const void* buf, size_t len);
ssize_t read(int fd, void* buf, size_t len);

int close(int fd);
int dup(int oldfd);
int dup2(int oldfd, int newfd);

// IPC primitives
int pipe(int pipefd[2]);
int mkfifo(const char* path, mode_t mode);

// Explicit FD inheritance controls for spawn/run workflows.
int fd_set_inherit(int enabled);
int fd_set_stdio(int stdin_fd, int stdout_fd, int stderr_fd);
int fd_set_nonblock(int fd, int enabled);

#define WNOHANG 1
int spawn(const char* path, const char* const* argv, int argc);
int waitpid(int pid, int* status, int options);

// Create/overwrite a file with given contents.
int writefile(const char* path, const void* buf, size_t len);

// Filesystem mutation helpers.
int mkdir(const char* path, mode_t mode);  /* mode is accepted but ignored on EYN-OS */
int unlink(const char* path);
int rmdir(const char* path);

/* access() checks existence (F_OK) or basic read/exec permission (R_OK/X_OK).
 * EYN-OS has no permission bits, so any accessible path returns 0. */
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1
int access(const char* path, int mode);

void _exit(int code) EYN_ATTR_NORETURN;

// Non-blocking single-key read. Returns 0 if none available.
int getkey(void);

// Sleep helpers (cooperative).
int usleep(unsigned int usec);
unsigned int sleep(unsigned int seconds);

// Query current working directory (shell/vterm cwd).
// Returns bytes written excluding NUL, or -1 on error.
int getcwd(char* buf, size_t size);

// Change current working directory (shell/vterm cwd).
// Returns 0 on success, -1 on error.
int chdir(const char* path);

/*
 * ABI-INVARIANT: SEEK_* values match POSIX and SYSCALL_LSEEK whence (110).
 */
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/* Reposition the offset of an open file descriptor. */
long lseek(int fd, long offset, int whence);

// Low-memory streaming file writer (EYNFS only today).
int eynfs_stream_begin(const char* path);
ssize_t eynfs_stream_write(int handle, const void* buf, size_t len);
int eynfs_stream_end(int handle);
