#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>

static int write_all(const char* s, size_t n) {
    if (!s) return -1;
    size_t pos = 0;
    while (pos < n) {
        ssize_t r = write(1, s + pos, n - pos);
        if (r <= 0) return -1;
        pos += (size_t)r;
    }
    return (int)n;
}

int putchar(int ch) {
    char c = (char)ch;
    if (write_all(&c, 1) < 0) return -1;
    return (unsigned char)c;
}

int puts(const char* s) {
    if (!s) return -1;
    int a = write_all(s, strlen(s));
    int b = write_all("\n", 1);
    if (a < 0 || b < 0) return -1;
    return a + b;
}

static int u32_to_buf(unsigned int v, unsigned int base, char* out, int out_cap) {
    const char* digits = "0123456789abcdef";
    char tmp[32];
    int n = 0;

    if (base < 2 || base > 16) return 0;
    if (v == 0) {
        if (out_cap > 0) out[0] = '0';
        return (out_cap > 0) ? 1 : 0;
    }

    while (v && n < (int)sizeof(tmp)) {
        tmp[n++] = digits[v % base];
        v /= base;
    }

    int w = 0;
    while (n > 0 && w < out_cap) {
        out[w++] = tmp[--n];
    }
    return w;
}

int printf(const char* fmt, ...) {
    if (!fmt) return -1;

    va_list ap;
    va_start(ap, fmt);

    int total = 0;

    for (const char* p = fmt; *p; p++) {
        if (*p != '%') {
            if (putchar(*p) < 0) { total = -1; break; }
            total++;
            continue;
        }

        p++;
        if (!*p) break;

        if (*p == '%') {
            if (putchar('%') < 0) { total = -1; break; }
            total++;
            continue;
        }

        if (*p == 'c') {
            int ch = va_arg(ap, int);
            if (putchar(ch) < 0) { total = -1; break; }
            total++;
            continue;
        }

        if (*p == 's') {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            size_t n = strlen(s);
            if (write_all(s, n) < 0) { total = -1; break; }
            total += (int)n;
            continue;
        }

        if (*p == 'd') {
            int v = va_arg(ap, int);
            unsigned int uv = (v < 0) ? (unsigned int)(-v) : (unsigned int)v;
            if (v < 0) {
                if (putchar('-') < 0) { total = -1; break; }
                total++;
            }
            char buf[16];
            int n = u32_to_buf(uv, 10, buf, (int)sizeof(buf));
            if (write_all(buf, (size_t)n) < 0) { total = -1; break; }
            total += n;
            continue;
        }

        if (*p == 'u' || *p == 'x') {
            unsigned int v = va_arg(ap, unsigned int);
            unsigned int base = (*p == 'x') ? 16u : 10u;
            char buf[16];
            int n = u32_to_buf(v, base, buf, (int)sizeof(buf));
            if (write_all(buf, (size_t)n) < 0) { total = -1; break; }
            total += n;
            continue;
        }

        // Unknown specifier: print it literally.
        if (putchar('%') < 0) { total = -1; break; }
        if (putchar(*p) < 0) { total = -1; break; }
        total += 2;
    }

    va_end(ap);
    return total;
}
