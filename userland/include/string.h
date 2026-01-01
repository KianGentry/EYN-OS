#include <stddef.h>

size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);

void* memcpy(void* dst, const void* src, size_t n);
void* memset(void* dst, int v, size_t n);

