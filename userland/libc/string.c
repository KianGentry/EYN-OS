#include <string.h>
#include <stdint.h>

// malloc/free are provided by userland/libc/stdlib.c
void* malloc(size_t n);

static int cstr_ptr_is_invalid(const char* s) {
    if (!s) return 1;
    /*
     * User mappings start well above the first page in EYN-OS.
     * Treat near-null pointers as invalid to avoid crashing formatter paths
     * when buggy callers pass values like 0x2c as a string pointer.
     */
    return ((uintptr_t)s < 0x1000u);
}

size_t strlen(const char* s) {
    size_t n = 0;
    if (cstr_ptr_is_invalid(s)) return 0;
    while (s[n]) n++;
    return n;
}

size_t strnlen(const char* s, size_t maxlen) {
    if (cstr_ptr_is_invalid(s)) return 0;
    size_t n = 0;
    while (n < maxlen && s[n]) n++;
    return n;
}

int strcmp(const char* a, const char* b) {
    if (a == b) return 0;
    if (cstr_ptr_is_invalid(a)) return -1;
    if (cstr_ptr_is_invalid(b)) return 1;
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    if (n == 0) return 0;
    if (a == b) return 0;
    if (cstr_ptr_is_invalid(a)) return -1;
    if (cstr_ptr_is_invalid(b)) return 1;
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char* strcpy(char* dst, const char* src) {
    if (!dst) return dst;
    if (!src) { dst[0] = '\0'; return dst; }
    char* d = dst;
    while ((*d++ = *src++)) { }
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n) {
    if (!dst || n == 0) return dst;
    size_t i = 0;
    if (src) {
        for (; i < n && src[i]; i++) dst[i] = src[i];
    }
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char* strcat(char* dst, const char* src) {
    if (!dst) return dst;
    size_t dlen = strlen(dst);
    strcpy(dst + dlen, src ? src : "");
    return dst;
}

char* strncat(char* dst, const char* src, size_t n) {
    if (!dst) return dst;
    size_t dlen = strlen(dst);
    if (!src || n == 0) return dst;
    size_t i = 0;
    while (i < n && src[i]) {
        dst[dlen + i] = src[i];
        i++;
    }
    dst[dlen + i] = '\0';
    return dst;
}

char* strchr(const char* s, int c) {
    if (!s) return 0;
    char ch = (char)c;
    for (; *s; s++) {
        if (*s == ch) return (char*)s;
    }
    if (ch == '\0') return (char*)s;
    return 0;
}

char* strrchr(const char* s, int c) {
    if (!s) return 0;
    char ch = (char)c;
    const char* last = 0;
    for (; *s; s++) {
        if (*s == ch) last = s;
    }
    if (ch == '\0') return (char*)s;
    return (char*)last;
}

static int starts_with(const char* s, const char* prefix) {
    if (!s || !prefix) return 0;
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

char* strstr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return 0;
    if (!*needle) return (char*)haystack;
    size_t nlen = strlen(needle);
    for (const char* p = haystack; *p; p++) {
        if (*p == *needle && starts_with(p, needle))
            return (char*)p;
        // Quick skip if remaining shorter than needle
        (void)nlen;
    }
    return 0;
}

void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

void* memset(void* dst, int v, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    unsigned char b = (unsigned char)v;
    for (size_t i = 0; i < n; i++) d[i] = b;
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* pa = (const unsigned char*)a;
    const unsigned char* pb = (const unsigned char*)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

char* strdup(const char* s) {
    if (!s) return 0;
    size_t n = strlen(s);
    char* p = (char*)malloc(n + 1);
    if (!p) return 0;
    memcpy(p, s, n + 1);
    return p;
}

char* strndup(const char* s, size_t n) {
    if (!s) return 0;
    size_t m = strnlen(s, n);
    char* p = (char*)malloc(m + 1);
    if (!p) return 0;
    memcpy(p, s, m);
    p[m] = '\0';
    return p;
}

static unsigned char tolower_ascii(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return (unsigned char)(c - 'A' + 'a');
    return c;
}

int strncasecmp(const char* a, const char* b, size_t n) {
    if (n == 0) return 0;
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = tolower_ascii((unsigned char)a[i]);
        unsigned char cb = tolower_ascii((unsigned char)b[i]);
        if (ca != cb) return (int)ca - (int)cb;
        if (a[i] == '\0') return 0;
    }
    return 0;
}

int strcasecmp(const char* a, const char* b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (*a && *b) {
        unsigned char ca = tolower_ascii((unsigned char)*a);
        unsigned char cb = tolower_ascii((unsigned char)*b);
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return tolower_ascii((unsigned char)*a) - tolower_ascii((unsigned char)*b);
}

static int is_delim(char c, const char* delim) {
    if (!delim) return 0;
    for (const char* p = delim; *p; p++) {
        if (*p == c) return 1;
    }
    return 0;
}

char* strtok(char* str, const char* delim) {
    static char* s_next = 0;
    if (str) s_next = str;
    if (!s_next) return 0;

    // Skip leading delimiters
    while (*s_next && is_delim(*s_next, delim)) s_next++;
    if (!*s_next) { s_next = 0; return 0; }

    char* tok = s_next;
    while (*s_next && !is_delim(*s_next, delim)) s_next++;
    if (*s_next) {
        *s_next = '\0';
        s_next++;
    } else {
        s_next = 0;
    }
    return tok;
}
