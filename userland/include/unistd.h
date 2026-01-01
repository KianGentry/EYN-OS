#include <stddef.h>

typedef long ssize_t;

// EYN-OS supports fd=0 (stdin), fd=1 (stdout) today.
ssize_t write(int fd, const void* buf, size_t len);
ssize_t read(int fd, void* buf, size_t len);

int close(int fd);

__attribute__((noreturn)) void _exit(int code);

// Non-blocking single-key read. Returns 0 if none available.
int getkey(void);
