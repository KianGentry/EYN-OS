#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

struct FILE {
    int kind;
    int fd;
    char* path;

    char* buf;
    size_t len;
    size_t cap;

    char** mem_bufp;
    size_t* mem_lenp;

    int err;
};

enum {
    FILE_KIND_FD = 1,
    FILE_KIND_MEM = 2,
    FILE_KIND_PATHWRITE = 3,
};

static struct FILE g_stdin = { .kind = FILE_KIND_FD, .fd = 0 };
static struct FILE g_stdout = { .kind = FILE_KIND_FD, .fd = 1 };
static struct FILE g_stderr = { .kind = FILE_KIND_FD, .fd = 1 };

FILE* stdin = &g_stdin;
FILE* stdout = &g_stdout;
FILE* stderr = &g_stderr;

static int file_write_bytes(FILE* f, const void* p, size_t n) {
    if (!f || !p) return -1;
    if (n == 0) return 0;

    if (f->kind == FILE_KIND_FD) {
        // Only stdout is supported by the kernel today.
        if (f->fd != 1) return -1;
        size_t pos = 0;
        while (pos < n) {
            size_t chunk = n - pos;
            if (chunk > 0x7fffffffU) chunk = 0x7fffffffU;
            ssize_t r = write(1, (const char*)p + pos, chunk);
            if (r <= 0) return -1;
            pos += (size_t)r;
        }
        return 0;
    }

    // Buffer-backed (memstream or pathwrite)
    if (f->len + n + 1 > f->cap) {
        size_t newcap = f->cap ? f->cap : 256;
        while (newcap < f->len + n + 1) newcap *= 2;
        char* nb = (char*)realloc(f->buf, newcap);
        if (!nb) return -1;
        f->buf = nb;
        f->cap = newcap;
    }
    memcpy(f->buf + f->len, p, n);
    f->len += n;
    f->buf[f->len] = '\0';

    if (f->kind == FILE_KIND_MEM && f->mem_bufp && f->mem_lenp) {
        *f->mem_bufp = f->buf;
        *f->mem_lenp = f->len;
    }
    return 0;
}

static int file_putc(FILE* f, int c) {
    unsigned char ch = (unsigned char)c;
    return file_write_bytes(f, &ch, 1);
}

FILE* fopen(const char* path, const char* mode) {
    if (!mode) return NULL;

    if (!path || strcmp(path, "-") == 0) {
        // Treat "-" as stdout for write, stdin for read.
        if (mode[0] == 'r') return stdin;
        if (mode[0] == 'w') return stdout;
        return NULL;
    }

    if (mode[0] == 'r') {
        int fd = open(path, O_RDONLY, 0);
        if (fd < 0) return NULL;
        FILE* f = (FILE*)calloc(1, sizeof(FILE));
        if (!f) { close(fd); return NULL; }
        f->kind = FILE_KIND_FD;
        f->fd = fd;
        return f;
    }

    if (mode[0] == 'w') {
        FILE* f = (FILE*)calloc(1, sizeof(FILE));
        if (!f) return NULL;
        f->kind = FILE_KIND_PATHWRITE;
        f->path = strdup(path);
        if (!f->path) { free(f); return NULL; }
        return f;
    }

    return NULL;
}

int fclose(FILE* f) {
    if (!f) return EOF;
    if (f == stdin || f == stdout || f == stderr) return 0;

    int rc = 0;
    if (f->kind == FILE_KIND_FD) {
        if (f->fd > 2) rc = close(f->fd);
    } else if (f->kind == FILE_KIND_PATHWRITE) {
        if (!f->path) rc = EOF;
        else {
            int w = writefile(f->path, f->buf ? f->buf : "", f->len);
            if (w < 0) rc = EOF;
        }
    } else if (f->kind == FILE_KIND_MEM) {
        // Caller owns the buffer.
    }

    if (f->kind != FILE_KIND_MEM) {
        if (f->buf) free(f->buf);
    }
    if (f->path) free(f->path);
    free(f);
    return rc;
}

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* f) {
    if (!ptr || !f) return 0;
    if (size == 0 || nmemb == 0) return 0;
    if (f->kind != FILE_KIND_FD) return 0;

    size_t total = size * nmemb;
    if (size != 0 && total / size != nmemb) return 0;

    ssize_t n = read(f->fd, ptr, total);
    if (n <= 0) return 0;
    return (size_t)n / size;
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* f) {
    if (!ptr || !f) return 0;
    if (size == 0 || nmemb == 0) return 0;

    size_t total = size * nmemb;
    if (size != 0 && total / size != nmemb) return 0;
    if (file_write_bytes(f, ptr, total) != 0) return 0;
    return nmemb;
}

int fputc(int c, FILE* f) {
    return (file_putc(f, c) == 0) ? c : EOF;
}

int fputs(const char* s, FILE* f) {
    if (!s) return EOF;
    size_t n = strlen(s);
    return (file_write_bytes(f, s, n) == 0) ? (int)n : EOF;
}

int fflush(FILE* f) {
    (void)f;
    return 0;
}

FILE* open_memstream(char** bufp, size_t* sizep) {
    if (!bufp || !sizep) return NULL;

    FILE* f = (FILE*)calloc(1, sizeof(FILE));
    if (!f) return NULL;
    f->kind = FILE_KIND_MEM;
    f->mem_bufp = bufp;
    f->mem_lenp = sizep;
    *bufp = NULL;
    *sizep = 0;
    return f;
}

static int u64_to_buf(uint64_t v, unsigned base, char* out, int out_cap) {
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
    while (n > 0 && w < out_cap) out[w++] = tmp[--n];
    return w;
}

static int write_padding(FILE* f, int count) {
    for (int i = 0; i < count; i++) {
        if (file_putc(f, ' ') != 0) return -1;
    }
    return 0;
}

int vfprintf(FILE* f, const char* fmt, va_list ap) {
    if (!f || !fmt) return -1;
    int total = 0;

    // Console color control: if the format starts with "%c", consume 3 ints
    // (r,g,b) and emit a control sequence understood by the kernel's user
    // console output path.
    if (fmt[0] == '%' && fmt[1] == 'c') {
        int r = va_arg(ap, int);
        int g = va_arg(ap, int);
        int b = va_arg(ap, int);

        if (f->kind == FILE_KIND_FD && f->fd == 1) {
            unsigned char ctrl[4];
            ctrl[0] = 0xFF;
            ctrl[1] = (unsigned char)r;
            ctrl[2] = (unsigned char)g;
            ctrl[3] = (unsigned char)b;
            if (file_write_bytes(f, ctrl, sizeof(ctrl)) != 0) return -1;
        }

        fmt += 2;
    }

    for (const char* p = fmt; *p; p++) {
        if (*p != '%') {
            if (file_putc(f, *p) != 0) return -1;
            total++;
            continue;
        }

        p++;
        if (!*p) break;

        int width = 0;
        int precision = -1;

        // Width: number or '*'
        if (*p == '*') {
            width = va_arg(ap, int);
            p++;
        } else {
            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p - '0');
                p++;
            }
        }

        // Precision: .number or .* (only used for %s)
        if (*p == '.') {
            p++;
            precision = 0;
            if (*p == '*') {
                precision = va_arg(ap, int);
                p++;
            } else {
                while (*p >= '0' && *p <= '9') {
                    precision = precision * 10 + (*p - '0');
                    p++;
                }
            }
        }

        // Length modifiers: l/ll (enough for chibicc's %ld/%lu)
        int is_long = 0;
        int is_longlong = 0;
        if (*p == 'l') {
            is_long = 1;
            p++;
            if (*p == 'l') {
                is_longlong = 1;
                p++;
            }
        }

        if (*p == '%') {
            if (file_putc(f, '%') != 0) return -1;
            total++;
            continue;
        }

        if (*p == 'c') {
            int ch = va_arg(ap, int);
            if (file_putc(f, ch) != 0) return -1;
            total++;
            continue;
        }

        if (*p == 's') {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            size_t slen = strlen(s);
            if (precision >= 0 && (size_t)precision < slen) slen = (size_t)precision;

            int pad = width - (int)slen;
            if (pad > 0) {
                if (write_padding(f, pad) != 0) return -1;
                total += pad;
            }

            if (file_write_bytes(f, s, slen) != 0) return -1;
            total += (int)slen;
            continue;
        }

        if (*p == 'd' || *p == 'u' || *p == 'x' || *p == 'p') {
            uint64_t uv = 0;
            int is_signed = (*p == 'd');
            int base = (*p == 'x' || *p == 'p') ? 16 : 10;

            int neg = 0;
            if (is_signed) {
                long long sv = is_longlong ? va_arg(ap, long long) : (long long)(is_long ? va_arg(ap, long) : va_arg(ap, int));
                if (sv < 0) { neg = 1; uv = (uint64_t)(-sv); }
                else uv = (uint64_t)sv;
            } else {
                uv = is_longlong ? va_arg(ap, unsigned long long) : (uint64_t)(is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int));
            }

            char num[32];
            int nlen = u64_to_buf(uv, (unsigned)base, num, (int)sizeof(num));
            int extra = 0;
            if (*p == 'p') {
                // 0x prefix
                extra = 2;
            }
            int sign = neg ? 1 : 0;
            int totlen = sign + extra + nlen;
            int pad = width - totlen;
            if (pad > 0) {
                if (write_padding(f, pad) != 0) return -1;
                total += pad;
            }

            if (neg) { if (file_putc(f, '-') != 0) return -1; total++; }
            if (*p == 'p') { if (file_putc(f, '0') != 0) return -1; if (file_putc(f, 'x') != 0) return -1; total += 2; }
            if (file_write_bytes(f, num, (size_t)nlen) != 0) return -1;
            total += nlen;
            continue;
        }

        // Unknown: print literally
        if (file_putc(f, '%') != 0) return -1;
        if (file_putc(f, *p) != 0) return -1;
        total += 2;
    }

    return total;
}

int fprintf(FILE* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = vfprintf(f, fmt, ap);
    va_end(ap);
    return rc;
}

int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return rc;
}

int putchar(int ch) {
    return fputc(ch, stdout);
}

int puts(const char* s) {
    if (!s) return EOF;
    if (fputs(s, stdout) == EOF) return EOF;
    if (fputc('\n', stdout) == EOF) return EOF;
    return 0;
}
