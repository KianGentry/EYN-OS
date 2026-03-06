#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

// Simple bump allocator with size headers.
// free() is a no-op; realloc() allocates a new block and copies.

#ifndef USERLAND_HEAP_SIZE
#define USERLAND_HEAP_SIZE (1024 * 1024)
#endif

static uint8_t g_heap[USERLAND_HEAP_SIZE];
static size_t g_heap_used = 0;
static uint32_t g_rand_state = 1u;
static int g_rand_seeded = 0;

static uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static uint32_t entropy_seed(void) {
    uint32_t t_lo = 0;
    uint32_t t_hi = 0;
#if (defined(__i386__) || defined(__x86_64__)) && !defined(__chibicc__)
    __asm__ __volatile__("rdtsc" : "=a"(t_lo), "=d"(t_hi));
#endif
    uint32_t addr_mix = (uint32_t)(uintptr_t)&g_heap_used ^ (uint32_t)(uintptr_t)&t_lo;
    uint32_t seed = t_lo ^ (t_hi * 1664525u) ^ addr_mix ^ 0x9e3779b9u;
    seed = mix32(seed);
    if (seed == 0) seed = 1u;
    return seed;
}

static size_t align_up(size_t v, size_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

void* malloc(size_t n) {
    if (n == 0) n = 1;

    // Header stores requested size.
    size_t total = sizeof(uint32_t) + n;
    total = align_up(total, 8);

    if (g_heap_used + total > sizeof(g_heap)) {
        errno = ENOMEM;
        return NULL;
    }

    uint8_t* p = &g_heap[g_heap_used];
    *(uint32_t*)p = (uint32_t)n;
    void* ret = p + sizeof(uint32_t);
    g_heap_used += total;
    return ret;
}

void free(void* p) {
    (void)p;
}

void* calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return malloc(1);
    size_t n = nmemb * size;
    if (size != 0 && n / size != nmemb) {
        errno = ENOMEM;
        return NULL;
    }
    void* p = malloc(n);
    if (!p) return NULL;
    memset(p, 0, n);
    return p;
}

void* realloc(void* p, size_t n) {
    if (!p) return malloc(n);
    if (n == 0) n = 1;

    uint8_t* raw = (uint8_t*)p - sizeof(uint32_t);
    uint32_t oldsz = *(uint32_t*)raw;

    void* np = malloc(n);
    if (!np) return NULL;

    size_t copy = oldsz < n ? oldsz : n;
    memcpy(np, p, copy);
    return np;
}

int atexit(void (*fn)(void)) {
    (void)fn;
    return 0;
}

char* getenv(const char* name) {
    (void)name;
    return NULL;
}

void abort(void) {
    _exit(1);
}

void exit(int code) {
    _exit(code);
}

void srand(unsigned int seed) {
    g_rand_state = (uint32_t)seed;
    if (g_rand_state == 0) g_rand_state = 1u;
    g_rand_seeded = 1;
}

int rand(void) {
    if (!g_rand_seeded) {
        srand(entropy_seed());
    }
    g_rand_state = g_rand_state * 1103515245u + 12345u;
    return (int)((g_rand_state >> 16) & RAND_MAX);
}

unsigned long strtoul(const char* nptr, char** endptr, int base) {
    if (!nptr) {
        if (endptr) *endptr = (char*)nptr;
        return 0;
    }

    const char* p = nptr;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    if (base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
        else if (p[0] == '0') { base = 8; p += 1; }
        else base = 10;
    } else if (base == 16) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    }

    unsigned long v = 0;
    for (;;) {
        int d;
        char c = *p;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned long)base + (unsigned long)d;
        p++;
    }

    if (endptr) *endptr = (char*)p;
    return v;
}

static int is_space_char(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * chibicc i386: long double storage not supported in codegen; define strtold
 * type as double via preprocessor. On i386, long double and double are both
 * 64-bit in practice for chibicc, so this is ABI-compatible.
 */
#ifdef __chibicc__
#define long_double double
#else
#define long_double long double
#endif

long_double strtold(const char* nptr, char** endptr) {
    const char* p = nptr;
    if (!p) {
        if (endptr) *endptr = (char*)nptr;
        return 0.0;
    }

    while (is_space_char(*p)) p++;

    int sign = 1;
    if (*p == '+') {
        p++;
    } else if (*p == '-') {
        sign = -1;
        p++;
    }

    const char* start = p;

    // Hex float: 0x...p...
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;

        long_double mant = 0.0;
        int frac_digits = 0;
        int saw_digit = 0;

        while (1) {
            int d = hex_digit(*p);
            if (d < 0) break;
            saw_digit = 1;
            mant = mant * 16.0 + (long_double)d;
            p++;
        }

        if (*p == '.') {
            p++;
            while (1) {
                int d = hex_digit(*p);
                if (d < 0) break;
                saw_digit = 1;
                mant = mant * 16.0 + (long_double)d;
                frac_digits++;
                p++;
            }
        }

        if (!saw_digit) {
            if (endptr) *endptr = (char*)nptr;
            return 0.0;
        }

        // Scale down by 16^frac_digits.
        for (int i = 0; i < frac_digits && i < 1024; i++)
            mant /= 16.0;

        int exp2 = 0;
        if (*p == 'p' || *p == 'P') {
            p++;
            int esign = 1;
            if (*p == '+') p++;
            else if (*p == '-') { esign = -1; p++; }

            int any = 0;
            while (*p >= '0' && *p <= '9') {
                any = 1;
                exp2 = exp2 * 10 + (*p - '0');
                p++;
                if (exp2 > 4096) exp2 = 4096;
            }
            if (!any) {
                // Invalid exponent; stop before 'p' token.
                p--; 
            } else {
                exp2 *= esign;
            }
        }

        if (exp2 > 0) {
            for (int i = 0; i < exp2 && i < 1024; i++)
                mant *= 2.0;
        } else if (exp2 < 0) {
            for (int i = 0; i < -exp2 && i < 1024; i++)
                mant /= 2.0;
        }

        if (endptr) *endptr = (char*)p;
        return (long_double)sign * mant;
    }

    // Decimal float.
    long_double val = 0.0;
    int saw_digit = 0;

    while (*p >= '0' && *p <= '9') {
        saw_digit = 1;
        val = val * 10.0 + (long_double)(*p - '0');
        p++;
    }

    int frac = 0;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            saw_digit = 1;
            val = val * 10.0 + (long_double)(*p - '0');
            frac++;
            p++;
            if (frac > 1024) frac = 1024;
        }
    }

    if (!saw_digit) {
        if (endptr) *endptr = (char*)nptr;
        return 0.0;
    }

    for (int i = 0; i < frac; i++)
        val /= 10.0;

    int exp10 = 0;
    if (*p == 'e' || *p == 'E') {
        p++;
        int esign = 1;
        if (*p == '+') p++;
        else if (*p == '-') { esign = -1; p++; }

        int any = 0;
        while (*p >= '0' && *p <= '9') {
            any = 1;
            exp10 = exp10 * 10 + (*p - '0');
            p++;
            if (exp10 > 4096) exp10 = 4096;
        }
        if (any) exp10 *= esign;
        else {
            // Roll back if exponent has no digits.
            p--;
            exp10 = 0;
        }
    }

    if (exp10 > 0) {
        for (int i = 0; i < exp10 && i < 1024; i++)
            val *= 10.0;
    } else if (exp10 < 0) {
        for (int i = 0; i < -exp10 && i < 1024; i++)
            val /= 10.0;
    }

    if (endptr) *endptr = (char*)p;
    (void)start;
    return (long_double)sign * val;
}

int atoi(const char* s) {
    if (!s) return 0;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    int val = 0;
    while (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; }
    return neg ? -val : val;
}

long atol(const char* s) {
    return (long)atoi(s);
}

int abs(int x) {
    return x < 0 ? -x : x;
}

long labs(long x) {
    return x < 0 ? -x : x;
}

long strtol(const char* nptr, char** endptr, int base) {
    return (long)strtoul(nptr, endptr, base);
}

double strtod(const char* nptr, char** endptr) {
    return (double)strtold(nptr, endptr);
}
