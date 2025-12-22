#include <string.h>

size_t strlen(const char* s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char* a, const char* b) {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    if (n == 0) return 0;
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void* memset(void* dst, int v, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    unsigned char b = (unsigned char)v;
    for (size_t i = 0; i < n; i++) d[i] = b;
    return dst;
}
