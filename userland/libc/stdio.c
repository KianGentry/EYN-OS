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

    /*
     * pos: cached byte offset within the file, kept in sync with the kernel
     *      user_fd_t.offset via SYSCALL_LSEEK.  Only meaningful for
     *      FILE_KIND_FD with a real file fd (not stdin/stdout).
     * eof_flag: set when fread() returns fewer bytes than requested due to
     *           end-of-file; cleared by fseek()/rewind().
     */
    long pos;
    int  eof_flag;
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
    if (n <= 0) {
        f->eof_flag = 1;
        return 0;
    }
    f->pos += n;
    /* If we got fewer bytes than requested, we've hit EOF. */
    if ((size_t)n < total) f->eof_flag = 1;
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

int fgetc(FILE* f) {
    if (!f) return EOF;
    unsigned char ch;
    if (fread(&ch, 1, 1, f) != 1) return EOF;
    return (int)ch;
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

/* ------------------------------------------------------------------ */
/* vsnprintf / snprintf – format into a bounded char buffer.          */
/* ------------------------------------------------------------------ */

/* Helper: append a single character to the snprintf output buffer.   */
static void snbuf_putc(char* buf, size_t sz, size_t* pos, char c) {
    if (*pos + 1 < sz) buf[*pos] = c;
    (*pos)++;
}

/* Helper: append a NUL-terminated string, honoring precision.        */
static void snbuf_puts(char* buf, size_t sz, size_t* pos,
                        const char* s, int width, int precision) {
    if (!s) s = "(null)";
    int len = 0;
    while (s[len]) len++;
    if (precision >= 0 && len > precision) len = precision;
    int pad = (width > len) ? width - len : 0;
    while (pad-- > 0) snbuf_putc(buf, sz, pos, ' ');
    for (int i = 0; i < len; i++) snbuf_putc(buf, sz, pos, s[i]);
}

/* Helper: render an unsigned value into a small stack buffer.        */
static void snbuf_render_unsigned(char tmp[24], int* len, unsigned long val, int base, int uppercase) {
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    *len = 0;
    if (val == 0) { tmp[(*len)++] = '0'; return; }
    while (val) {
        tmp[(*len)++] = digits[val % (unsigned)base];
        val /= (unsigned)base;
    }
    /* reverse */
    for (int i = 0, j = *len - 1; i < j; i++, j--) {
        char t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t;
    }
}

int vsnprintf(char* buf, size_t sz, const char* fmt, va_list ap) {
    if (!fmt) { if (buf && sz) buf[0] = '\0'; return 0; }
    size_t pos = 0;

    for (const char* p = fmt; *p; p++) {
        if (*p != '%') { snbuf_putc(buf, sz, &pos, *p); continue; }
        p++;
        if (!*p) break;

        /* Flags */
        int left_align = 0, zero_pad = 0, force_sign = 0;
        for (;;) {
            if (*p == '-')       { left_align = 1; p++; }
            else if (*p == '0') { zero_pad = 1;   p++; }
            else if (*p == '+') { force_sign = 1;  p++; }
            else break;
        }

        /* Width */
        int width = 0;
        if (*p == '*') { width = va_arg(ap, int); p++; }
        else while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }

        /* Precision */
        int precision = -1;
        if (*p == '.') {
            p++; precision = 0;
            if (*p == '*') { precision = va_arg(ap, int); p++; }
            else while (*p >= '0' && *p <= '9') { precision = precision * 10 + (*p - '0'); p++; }
        }

        /* Length modifier */
        int is_long = 0;
        if (*p == 'l') { is_long = 1; p++; if (*p == 'l') { p++; } }
        else if (*p == 'z') { is_long = 1; p++; }

        if (!*p) break;

        if (*p == '%') {
            snbuf_putc(buf, sz, &pos, '%');
        } else if (*p == 'c') {
            char ch = (char)va_arg(ap, int);
            snbuf_putc(buf, sz, &pos, ch);
        } else if (*p == 's') {
            const char* s = va_arg(ap, const char*);
            snbuf_puts(buf, sz, &pos, s, left_align ? -width : width, precision);
        } else if (*p == 'd' || *p == 'i') {
            long val = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
            char tmp[24]; int tlen = 0;
            int neg = 0;
            unsigned long uv;
            if (val < 0) { neg = 1; uv = (unsigned long)(-(val + 1)) + 1; } else { uv = (unsigned long)val; }
            snbuf_render_unsigned(tmp, &tlen, uv, 10, 0);
            /* precision for %d = minimum digit count; zero-pad between sign and digits */
            int prec_zeros = (precision > tlen) ? precision - tlen : 0;
            int numw = tlen + prec_zeros + neg + (force_sign && !neg ? 1 : 0);
            int pad = (width > numw) ? width - numw : 0;
            char padch = (zero_pad && !left_align && precision < 0) ? '0' : ' ';
            if (!left_align && padch == ' ') while (pad-- > 0) snbuf_putc(buf, sz, &pos, ' ');
            if (neg) snbuf_putc(buf, sz, &pos, '-');
            else if (force_sign) snbuf_putc(buf, sz, &pos, '+');
            if (!left_align && padch == '0') while (pad-- > 0) snbuf_putc(buf, sz, &pos, '0');
            for (int i = 0; i < prec_zeros; i++) snbuf_putc(buf, sz, &pos, '0');
            for (int i = 0; i < tlen; i++) snbuf_putc(buf, sz, &pos, tmp[i]);
            if (left_align) while (pad-- > 0) snbuf_putc(buf, sz, &pos, ' ');
        } else if (*p == 'u') {
            unsigned long val = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            char tmp[24]; int tlen = 0;
            snbuf_render_unsigned(tmp, &tlen, val, 10, 0);
            int prec_zeros = (precision > tlen) ? precision - tlen : 0;
            int numw = tlen + prec_zeros;
            int pad = (width > numw) ? width - numw : 0;
            char padch = (zero_pad && !left_align && precision < 0) ? '0' : ' ';
            if (!left_align && padch == ' ') while (pad-- > 0) snbuf_putc(buf, sz, &pos, ' ');
            if (!left_align && padch == '0') while (pad-- > 0) snbuf_putc(buf, sz, &pos, '0');
            for (int i = 0; i < prec_zeros; i++) snbuf_putc(buf, sz, &pos, '0');
            for (int i = 0; i < tlen; i++) snbuf_putc(buf, sz, &pos, tmp[i]);
            if (left_align) while (pad-- > 0) snbuf_putc(buf, sz, &pos, ' ');
        } else if (*p == 'x' || *p == 'X') {
            unsigned long val = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            char tmp[24]; int tlen = 0;
            snbuf_render_unsigned(tmp, &tlen, val, 16, (*p == 'X'));
            int prec_zeros = (precision > tlen) ? precision - tlen : 0;
            int numw = tlen + prec_zeros;
            int pad = (width > numw) ? width - numw : 0;
            char padch = (zero_pad && !left_align && precision < 0) ? '0' : ' ';
            if (!left_align && padch == ' ') while (pad-- > 0) snbuf_putc(buf, sz, &pos, ' ');
            if (!left_align && padch == '0') while (pad-- > 0) snbuf_putc(buf, sz, &pos, '0');
            for (int i = 0; i < prec_zeros; i++) snbuf_putc(buf, sz, &pos, '0');
            for (int i = 0; i < tlen; i++) snbuf_putc(buf, sz, &pos, tmp[i]);
            if (left_align) while (pad-- > 0) snbuf_putc(buf, sz, &pos, ' ');
        } else if (*p == 'p') {
            unsigned long val = (unsigned long)(uintptr_t)va_arg(ap, void*);
            snbuf_putc(buf, sz, &pos, '0');
            snbuf_putc(buf, sz, &pos, 'x');
            char tmp[24]; int tlen = 0;
            snbuf_render_unsigned(tmp, &tlen, val, 16, 0);
            for (int i = 0; i < tlen; i++) snbuf_putc(buf, sz, &pos, tmp[i]);
        } else {
            /* Unknown specifier -- emit literally */
            snbuf_putc(buf, sz, &pos, '%');
            snbuf_putc(buf, sz, &pos, *p);
        }
    }

    /* NUL-terminate */
    if (buf && sz > 0) buf[pos < sz ? pos : sz - 1] = '\0';
    return (int)pos;
}

int snprintf(char* buf, size_t sz, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = vsnprintf(buf, sz, fmt, ap);
    va_end(ap);
    return rc;
}

int vfprintf(FILE* f, const char* fmt, va_list ap) {
    if (!f || !fmt) return -1;
    int total = 0;

    // Console colour control: if writing to stdout AND the format starts with
    // "%c", consume 3 ints (r,g,b) and emit a control sequence understood
    // by the kernel's console output path.
    //
    // IMPORTANT: only apply this special handling when f is stdout (fd==1).
    // For memstream / stderr / file output, %c must behave as standard C
    // (write a single character).  Getting this wrong breaks any code that
    // uses format("%c%s", ch, str) to build a plain string, because the
    // branch would silently consume 3 va_args instead of 1.
    if (f->kind == FILE_KIND_FD && f->fd == 1 &&
        fmt[0] == '%' && fmt[1] == 'c') {
        int r = va_arg(ap, int);
        int g = va_arg(ap, int);
        int b = va_arg(ap, int);
        unsigned char ctrl[4];
        ctrl[0] = 0xFF;
        ctrl[1] = (unsigned char)r;
        ctrl[2] = (unsigned char)g;
        ctrl[3] = (unsigned char)b;
        if (file_write_bytes(f, ctrl, sizeof(ctrl)) != 0) return -1;
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

        if (*p == 'd' || *p == 'i' || *p == 'u' || *p == 'x' || *p == 'p') {
            uint64_t uv = 0;
            int is_signed = (*p == 'd' || *p == 'i');
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
            /* precision for integers = minimum digit count; zero-pad if needed */
            int prec_zeros = (precision >= 0 && precision > nlen) ? precision - nlen : 0;
            int sign = neg ? 1 : 0;
            int totlen = sign + extra + prec_zeros + nlen;
            int pad = width - totlen;
            if (pad > 0) {
                if (write_padding(f, pad) != 0) return -1;
                total += pad;
            }

            if (neg) { if (file_putc(f, '-') != 0) return -1; total++; }
            if (*p == 'p') { if (file_putc(f, '0') != 0) return -1; if (file_putc(f, 'x') != 0) return -1; total += 2; }
            for (int zi = 0; zi < prec_zeros; zi++) { if (file_putc(f, '0') != 0) return -1; total++; }
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

int vsprintf(char* buf, const char* fmt, va_list ap) {
    /* Use vsnprintf with an absurdly large limit.
     * Callers are responsible for buffer sizing (this is the old C API). */
    return vsnprintf(buf, 65536, fmt, ap);
}

int sprintf(char* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

/* fscanf -- read one word/token from a FILE and parse it.
 *
 * Very minimal: reads a whitespace-delimited token into a local buffer and
 * delegates to vsscanf.  Only needed for M_LoadDefaults (reads one token per
 * call).  Does not support multi-field format strings well.
 */
int fscanf(FILE* f, const char* fmt, ...) {
    if (!f || !fmt) return EOF;
    /* Read a token (up to whitespace or end), then sscanf it. */
    char tok[256];
    int  pos = 0;
    int  ch;

    /* Skip leading whitespace. */
    while ((ch = fgetc(f)) != EOF) {
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
            tok[pos++] = (char)ch;
            break;
        }
    }
    if (pos == 0) return EOF;

    /* Read until whitespace or EOF. */
    while ((ch = fgetc(f)) != EOF) {
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') break;
        if (pos < (int)(sizeof(tok) - 1)) tok[pos++] = (char)ch;
    }
    tok[pos] = '\0';

    va_list ap;
    va_start(ap, fmt);
    int n = vsscanf(tok, fmt, ap);
    va_end(ap);
    return n;
}

/* setbuf -- DOOM calls this to disable buffering on stderr.
 * EYN-OS FILE is always write-through so this is a no-op. */
void setbuf(FILE* f, char* buf) {
    (void)f; (void)buf;
}

/* getchar -- read a single character from stdin. */
int getchar(void) {
    return fgetc(stdin);
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

/*
 * File-positioning functions backed by SYSCALL_LSEEK (110).
 *
 * Only FILE_KIND_FD files with actual file descriptors support seeking;
 * stdin/stdout/pathwrite return errors or no-ops as appropriate.
 *
 * The pos field in struct FILE mirrors the kernel-side offset so that
 * ftell() can return the position without an extra syscall.
 */
int fseek(FILE* f, long offset, int whence) {
    if (!f) return -1;
    if (f->kind != FILE_KIND_FD || f->fd <= 2) return -1;
    int result = lseek(f->fd, (int)offset, whence);
    if (result < 0) return -1;
    f->pos = (long)result;
    f->eof_flag = 0;  /* clear EOF on successful seek */
    return 0;
}

long ftell(FILE* f) {
    if (!f) return -1L;
    /* For real file fds, return the cached position. */
    if (f->kind == FILE_KIND_FD && f->fd > 2)
        return f->pos;
    return -1L;
}

void rewind(FILE* f) {
    if (!f) return;
    fseek(f, 0L, 0 /* SEEK_SET */);
    f->err = 0;
    f->eof_flag = 0;
}

int feof(FILE* f) {
    if (!f) return 1;
    return f->eof_flag;
}

int ferror(FILE* f) {
    if (!f) return 1;
    return f->err;
}

/*
 * vsscanf / sscanf -- minimal scanf implementation for reading from a string.
 *
 * Supports: %d %i %u %x %c %s and skips whitespace between conversions.
 * Does not support width specifiers, floats, or *, which are not needed
 * by the DOOM source code that exercises this path.
 */
int vsscanf(const char* str, const char* fmt, va_list ap) {
    const char* p  = str;
    const char* f  = fmt;
    int         n  = 0;  /* number of successful assignments */

    while (*f) {
        /* Skip whitespace in format → skip whitespace in input. */
        if (*f == ' ' || *f == '\t' || *f == '\n') {
            while (*p == ' ' || *p == '\t' || *p == '\n') p++;
            while (*f == ' ' || *f == '\t' || *f == '\n') f++;
            continue;
        }

        if (*f != '%') {
            /* Literal match. */
            if (*p != *f) break;
            p++; f++;
            continue;
        }
        f++;  /* skip '%' */

        /* Skip '*' (suppress assignment) -- basic support. */
        int suppress = 0;
        if (*f == '*') { suppress = 1; f++; }

        /* Skip optional width (not used by callers). */
        while (*f >= '0' && *f <= '9') f++;

        int base = 10;
        switch (*f) {
            case 'x': case 'X': base = 16; /* fall through */
            case 'i':
            case 'd': {
                /* Skip leading whitespace. */
                while (*p == ' ' || *p == '\t') p++;
                int neg = 0;
                if (*p == '-') { neg = 1; p++; }
                else if (*p == '+') p++;
                /* Detect hex prefix for %i. */
                if ((*f == 'i') && *p == '0' && (*(p+1)=='x'||*(p+1)=='X')) {
                    base = 16; p += 2;
                } else if ((*f == 'i') && *p == '0') {
                    base = 8;
                }
                if (!*p) goto done;
                long val = 0;
                const char* start = p;
                while (*p) {
                    int dig;
                    if (*p >= '0' && *p <= '9') dig = *p - '0';
                    else if (base == 16 && *p >= 'a' && *p <= 'f') dig = *p - 'a' + 10;
                    else if (base == 16 && *p >= 'A' && *p <= 'F') dig = *p - 'A' + 10;
                    else break;
                    if (dig >= base) break;
                    val = val * base + dig;
                    p++;
                }
                if (p == start) goto done;
                if (!suppress) { *va_arg(ap, int*) = (int)(neg ? -val : val); n++; }
                break;
            }
            case 'u': {
                while (*p == ' ' || *p == '\t') p++;
                unsigned long val = 0;
                const char* start = p;
                while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
                if (p == start) goto done;
                if (!suppress) { *va_arg(ap, unsigned int*) = (unsigned int)val; n++; }
                break;
            }
            case 'c': {
                if (!*p) goto done;
                if (!suppress) { *va_arg(ap, char*) = *p; n++; }
                p++;
                break;
            }
            case 's': {
                while (*p == ' ' || *p == '\t') p++;
                if (!*p) goto done;
                char* dst = suppress ? 0 : va_arg(ap, char*);
                while (*p && *p != ' ' && *p != '\t' && *p != '\n') {
                    if (dst) *dst++ = *p;
                    p++;
                }
                if (dst) { *dst = '\0'; n++; }
                break;
            }
            case '%': {
                if (*p != '%') goto done;
                p++;
                break;
            }
            default: goto done;
        }
        f++;
    }
done:
    return n;
}

int sscanf(const char* str, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsscanf(str, fmt, ap);
    va_end(ap);
    return n;
}