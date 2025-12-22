#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int putchar(int ch);
int puts(const char* s);

// Minimal printf: supports %s %c %d %u %x %%
int printf(const char* fmt, ...);

#ifdef __cplusplus
}
#endif
