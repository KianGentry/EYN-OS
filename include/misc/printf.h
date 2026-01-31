#ifndef EYNOS_MISC_PRINTF_H
#define EYNOS_MISC_PRINTF_H

#include <stddef.h>

// Kernel-provided printf/snprintf (freestanding; not libc).
void printf(const char* format, ...);
int snprintf(char* str, size_t size, const char* format, ...);

#endif
