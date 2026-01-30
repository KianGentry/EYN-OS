#include <misc/types.h>
#include <stddef.h>
#include <utilities/string.h>

/*
 * Freestanding string/memory routines for the AArch64 bring-up build.
 *
 * Notes:
 * - Keep implementations simple and alignment-safe.
 * - Avoid dependencies on VGA/serial so these routines can be used early.
 */

void* memcpy(void* dest, const void* src, size_t n) {
    if (!dest || !src || n == 0) return dest;

    uint8* d = (uint8*)dest;
    const uint8* s = (const uint8*)src;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dest;
}

void* memset(void* s, int c, size_t n) {
    if (!s || n == 0) return s;

    uint8* p = (uint8*)s;
    uint8 v = (uint8)c;
    for (size_t i = 0; i < n; ++i) p[i] = v;
    return s;
}

void* memmove(void* dest, const void* src, size_t n) {
    if (!dest || !src || n == 0) return dest;

    uint8* d = (uint8*)dest;
    const uint8* s = (const uint8*)src;

    if (s < d && (s + n) > d) {
        for (size_t i = n; i != 0; --i) {
            d[i - 1] = s[i - 1];
        }
        return dest;
    }

    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dest;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    if (!s1 || !s2 || n == 0) return 0;

    const uint8* a = (const uint8*)s1;
    const uint8* b = (const uint8*)s2;
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    }
    return 0;
}

size_t strlen(const char* s) {
    if (!s) return 0;
    const char* p = s;
    while (*p) ++p;
    return (size_t)(p - s);
}

uint16 strlength(string ch) {
    if (!ch) {
        return 0;
    }
    size_t n = strlen(ch);
    if (n > 0xFFFFu) {
        return 0xFFFFu;
    }
    return (uint16)n;
}

char* strcpy(char* dest, const char* src) {
    if (!dest || !src) return dest;
    char* d = dest;
    while ((*d++ = *src++) != '\0') {
    }
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n) {
    if (!dest || !src) return dest;

    size_t i = 0;
    for (; i < n && src[i]; ++i) dest[i] = src[i];
    for (; i < n; ++i) dest[i] = '\0';
    return dest;
}

char* strcat(char* dest, const char* src) {
    if (!dest || !src) return dest;

    char* d = dest;
    while (*d) ++d;
    while ((*d++ = *src++) != '\0') {
    }
    return dest;
}

char* strncat(char* dest, const char* src, size_t n) {
    if (!dest || !src) return dest;

    char* d = dest;
    while (*d) ++d;

    size_t i = 0;
    for (; i < n && src[i]; ++i) d[i] = src[i];
    d[i] = '\0';
    return dest;
}

int strcmp(const char* s1, const char* s2) {
    if (!s1 || !s2) return 0;

    while (*s1 && (*s1 == *s2)) {
        ++s1;
        ++s2;
    }
    return (int)(unsigned char)(*s1) - (int)(unsigned char)(*s2);
}

int strncmp(const char* s1, const char* s2, size_t n) {
    if (!s1 || !s2 || n == 0) return 0;

    for (size_t i = 0; i < n; ++i) {
        unsigned char a = (unsigned char)s1[i];
        unsigned char b = (unsigned char)s2[i];
        if (a != b) return (int)a - (int)b;
        if (a == 0) return 0;
    }
    return 0;
}

char* strchr(const char* s, int c) {
    if (!s) return NULL;
    char ch = (char)c;
    while (*s) {
        if (*s == ch) return (char*)s;
        ++s;
    }
    return (ch == '\0') ? (char*)s : NULL;
}

char* strrchr(const char* s, int c) {
    if (!s) return NULL;
    char ch = (char)c;
    const char* last = NULL;
    while (*s) {
        if (*s == ch) last = s;
        ++s;
    }
    if (ch == '\0') return (char*)s;
    return (char*)last;
}

char* strstr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return NULL;

    if (*needle == '\0') return (char*)haystack;

    size_t nlen = strlen(needle);
    for (const char* h = haystack; *h; ++h) {
        if (*h == *needle) {
            if (strncmp(h, needle, nlen) == 0) return (char*)h;
        }
    }

    return NULL;
}

size_t strspn(const char* s, const char* accept) {
    if (!s || !accept) return 0;

    size_t count = 0;
    while (*s) {
        const char* a = accept;
        int found = 0;
        while (*a) {
            if (*a == *s) {
                found = 1;
                break;
            }
            ++a;
        }
        if (!found) break;
        ++s;
        ++count;
    }
    return count;
}

char* strpbrk(const char* s, const char* accept) {
    if (!s || !accept) return NULL;

    while (*s) {
        const char* a = accept;
        while (*a) {
            if (*a == *s) return (char*)s;
            ++a;
        }
        ++s;
    }

    return NULL;
}

char* strtok_r(char* str, const char* delim, char** saveptr) {
    if (!delim || !saveptr) return NULL;

    char* s = str ? str : *saveptr;
    if (!s) return NULL;

    /* Skip leading delimiters */
    while (*s && strchr(delim, *s)) ++s;
    if (!*s) {
        *saveptr = NULL;
        return NULL;
    }

    char* token = s;
    while (*s && !strchr(delim, *s)) ++s;

    if (*s) {
        *s = '\0';
        ++s;
    }

    *saveptr = s;
    return token;
}

int atoi(const char* s) {
    if (!s) return 0;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;

    int sign = 1;
    if (*s == '-') {
        sign = -1;
        ++s;
    } else if (*s == '+') {
        ++s;
    }

    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        ++s;
    }
    return v * sign;
}

/*
 * The remaining EYN-OS-specific helpers in include/utilities/string.h are not
 * needed by the current AArch64 bring-up targets. When we start porting the full
 * shell and utilities over, we can either:
 * - move their implementations to an architecture-independent file, or
 * - provide AArch64 versions here.
 */
