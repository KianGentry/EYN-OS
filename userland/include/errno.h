#pragma once

// Minimal errno support.

extern int errno;

#define ENOENT  2
#define EINVAL  22
#define ENOMEM  12
#define ENOSYS  38
#define EBADF   9   /* bad file descriptor */
#define EACCES  13  /* permission denied */
#define EEXIST  17  /* file already exists */
#define EISDIR  21  /* is a directory */
#define ENOTDIR 20  /* not a directory */
#define EFAULT  14  /* bad address */
#define EIO     5   /* I/O error */

const char* strerror(int errnum);
