#include <stddef.h>

int putchar(int ch);
int puts(const char* s);

// Minimal printf: supports %s %c %d %u %x %%
int printf(const char* fmt, ...);
