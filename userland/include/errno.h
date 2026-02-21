#pragma once

// Minimal errno support.

extern int errno;

#define ENOENT 2
#define EINVAL 22
#define ENOMEM 12
#define ENOSYS 38

const char* strerror(int errnum);
