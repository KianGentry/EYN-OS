#include <misc/types.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

#include <drivers/aarch64/fb_simple.h>
#include <drivers/vga.h>

extern int shell_redirect_active;

/*
 * AArch64 console printf
 *
 * The i386 build provides printf/snprintf from the VGA driver. For the AArch64
 * bring-up build we want the same symbols without pulling in VGA code.
 *
 * Compatibility note:
 * - EYN-OS printf supports a leading "%c" that consumes 3 ints (r,g,b) to set
 *   color. On AArch64 we ignore color but still consume the arguments to keep
 *   existing call sites working.
 */

void uart_pl011_write(const char* s);

static void console_write(const char* s) {
    if (!s) return;

    /* If the shell has enabled output capture (used by pipelines/redirection),
     * append output to the redirect buffer instead of writing to the console.
     */
    if (shell_redirect_active) {
        while (*s && shell_redirect_pos < (SHELL_REDIRECT_BUF_SIZE - 1)) {
            char c = *s++;
            if (c == '\r') {
                continue;
            }
            shell_redirect_buf[shell_redirect_pos++] = c;
        }
        shell_redirect_buf[shell_redirect_pos] = 0;
        return;
    }

    /* UART first (single lock inside uart_pl011_write). */
    uart_pl011_write(s);

    /* Mirror to framebuffer, if available. */
    if (fb_simple_ready()) {
        while (*s) {
            char c = *s++;
            if (c == '\n') fb_simple_putc('\r');
            fb_simple_putc(c);
        }
    }
}

static void out_pad(char** outp, size_t* left, char c) {
    if (outp && *outp && left && *left > 1) {
        **outp = c;
        (*outp)++;
        (*left)--;
    }
}

static void out_str(char** outp, size_t* left, const char* s) {
    if (!s) s = "(null)";
    while (*s) {
        out_pad(outp, left, *s++);
    }
}

static void out_u64_dec(char** outp, size_t* left, uint64 v) {
    char tmp[32];
    int n = 0;
    do {
        uint64 q = v / 10u;
        uint64 r = v - q * 10u;
        tmp[n++] = (char)('0' + (char)r);
        v = q;
    } while (v != 0 && n < (int)sizeof(tmp));

    while (n-- > 0) {
        out_pad(outp, left, tmp[n]);
    }
}

static void out_u64_hex(char** outp, size_t* left, uint64 v, int upper, int min_digits) {
    static const char* hex_l = "0123456789abcdef";
    static const char* hex_u = "0123456789ABCDEF";
    const char* hex = upper ? hex_u : hex_l;

    char tmp[32];
    int n = 0;
    do {
        tmp[n++] = hex[(int)(v & 0xFu)];
        v >>= 4;
    } while (v != 0 && n < (int)sizeof(tmp));

    while (n < min_digits && n < (int)sizeof(tmp)) {
        tmp[n++] = '0';
    }

    while (n-- > 0) {
        out_pad(outp, left, tmp[n]);
    }
}

int vsnprintf(char* str, size_t size, const char* format, va_list ap) {
    if (!str || size == 0) {
        return 0;
    }

    char* outp = str;
    size_t left = size;

    const char* p = format;
    if (!p) {
        str[0] = '\0';
        return 0;
    }

    /* Ignore leading color triplet: printf("%c...", r,g,b, ...) */
    if (p[0] == '%' && p[1] == 'c') {
        (void)va_arg(ap, int);
        (void)va_arg(ap, int);
        (void)va_arg(ap, int);
        p += 2;
    }

    while (*p && left > 1) {
        if (*p != '%') {
            out_pad(&outp, &left, *p++);
            continue;
        }

        ++p;
        if (*p == '%') {
            out_pad(&outp, &left, '%');
            ++p;
            continue;
        }

        /* Minimal width support: %08x, %4d */
        int zero_pad = 0;
        int width = 0;
        if (*p == '0') {
            zero_pad = 1;
            ++p;
        }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            ++p;
        }

        switch (*p) {
            case 'c': {
                char c = (char)va_arg(ap, int);
                out_pad(&outp, &left, c);
                ++p;
                break;
            }
            case 's': {
                const char* s = va_arg(ap, const char*);
                out_str(&outp, &left, s);
                ++p;
                break;
            }
            case 'd':
            case 'i': {
                int64 v = (int64)va_arg(ap, int);
                if (v < 0) {
                    out_pad(&outp, &left, '-');
                    out_u64_dec(&outp, &left, (uint64)(-v));
                } else {
                    out_u64_dec(&outp, &left, (uint64)v);
                }
                ++p;
                break;
            }
            case 'u': {
                uint64 v = (uint64)va_arg(ap, unsigned int);
                out_u64_dec(&outp, &left, v);
                ++p;
                break;
            }
            case 'x':
            case 'X': {
                uint64 v = (uint64)va_arg(ap, unsigned int);
                int upper = (*p == 'X');
                int min_digits = (zero_pad && width > 0) ? width : 0;
                out_u64_hex(&outp, &left, v, upper, min_digits);
                ++p;
                break;
            }
            case 'p': {
                uintptr_t v = (uintptr_t)va_arg(ap, void*);
                out_pad(&outp, &left, '0');
                out_pad(&outp, &left, 'x');
                out_u64_hex(&outp, &left, (uint64)v, 0, (int)(sizeof(uintptr_t) * 2));
                ++p;
                break;
            }
            default: {
                /* Unknown specifier: emit it literally to aid debugging */
                out_pad(&outp, &left, '%');
                if (*p) {
                    out_pad(&outp, &left, *p);
                    ++p;
                }
                break;
            }
        }
    }

    *outp = '\0';
    return (int)(outp - str);
}

int snprintf(char* str, size_t size, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(str, size, format, ap);
    va_end(ap);
    return n;
}

void printf(const char* format, ...) {
    va_list ap;
    va_start(ap, format);

    /*
     * Format into a bounded buffer then write; avoids needing a streaming
     * formatter for now while keeping stack usage small.
     */
    char buf[512];
    (void)vsnprintf(buf, sizeof(buf), format, ap);
    console_write(buf);

    va_end(ap);
}
