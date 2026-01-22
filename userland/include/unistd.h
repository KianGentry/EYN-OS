#include <stddef.h>

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

// Create/overwrite a file with given contents.
int writefile(const char* path, const void* buf, size_t len);

void _exit(int code) EYN_ATTR_NORETURN;

// Non-blocking single-key read. Returns 0 if none available.
int getkey(void);

// Sleep helpers (cooperative).
int usleep(unsigned int usec);
unsigned int sleep(unsigned int seconds);
