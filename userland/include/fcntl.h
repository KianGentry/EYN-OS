/*
 * ABI-INVARIANT: open() flag values for EYN-OS.
 *
 * These must match the values decoded by SYSCALL_OPEN in the kernel.
 * O_RDONLY = 0 is the default: any non-write open with flags==0 reads only.
 * Write flags (O_WRONLY, O_RDWR) are accepted by the kernel but EYN-OS VFS
 * currently always opens writeable; the flag is recorded for future use.
 * O_CREAT | O_TRUNC control file-creation / truncation on write paths.
 * O_BINARY is a DOSISH flag accepted but ignored (all I/O is binary on EYN-OS).
 */
#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_CREAT   0x0040   /* create file if it does not exist */
#define O_TRUNC   0x0200   /* truncate file to zero on open   */
#define O_APPEND  0x0400   /* writes always go to end of file */
#define O_NONBLOCK 0x0800  /* non-blocking I/O for supported endpoints */
#define O_BINARY  0x0000   /* no-op on EYN-OS (all I/O is binary) */

int open(const char* path, int flags, ...);   /* varargs: optional mode_t mode */
int creat(const char* path, int mode);
