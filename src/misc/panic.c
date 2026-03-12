#include <panic.h>
#include <vga.h>
#include <serial.h>
#include <multiboot.h>
#include <system.h>
#include <arch.h>
#include <stdarg.h>
#include <string.h>

static void halt_forever(void) {
    arch_halt_forever();
}

static volatile int g_panic_in_progress = 0;

int panic_is_in_progress(void) { return g_panic_in_progress; }

// Render a Blue Screen of Diagnostics (BSOD) with details; avoid printf during panic
// (Logo intentionally omitted per design preference)

static unsigned fnv1a_hash(const char* a, const char* b2, int line) {
    unsigned int h = 2166136261u;
    if (a) { for (const char* p=a; *p; ++p) { h ^= (unsigned char)(*p); h *= 16777619u; } }
    if (b2) { for (const char* p=b2; *p; ++p) { h ^= (unsigned char)(*p); h *= 16777619u; } }
    h ^= (unsigned)line; h *= 16777619u; return h;
}

typedef enum { CAT_GENERIC=0, CAT_ASSERT, CAT_PAGING, CAT_FILESYSTEM, CAT_IRQ } panic_cat_t;

static panic_cat_t classify_category(const char* title, const char* msg) {
    // Simple keyword scan; future: pass explicit reason codes
    const char* t = title ? title : "";
    const char* m = msg ? msg : "";
    if (strstr(t, "ASSERT") || strstr(m, "assert")) return CAT_ASSERT;
    if (strstr(m, "page fault") || strstr(m, "CR2") || strstr(m, "paging")) return CAT_PAGING;
    if (strstr(m, "EYNFS") || strstr(m, "FAT32") || strstr(m, "filesystem")) return CAT_FILESYSTEM;
    if (strstr(m, "IRQ") || strstr(m, "IDT") || strstr(m, "ISR")) return CAT_IRQ;
    return CAT_GENERIC;
}

static const char* category_label(panic_cat_t c) {
    switch(c) {
        case CAT_GENERIC: return "GENERAL";
        case CAT_ASSERT: return "ASSERT";
        case CAT_PAGING: return "PAGING";
        case CAT_FILESYSTEM: return "FILESYSTEM";
        case CAT_IRQ: return "IRQ";
        default: return "GENERAL";
    }
}

static void pick_bg_colour_for_category(panic_cat_t c, unsigned hash, int* pr, int* pg, int* pb) {
    (void)c; (void)hash;
    // Dark, calm background similar to #3d4872
    *pr = 61; *pg = 72; *pb = 114;
}

static void render_bsod(const char* title, const char* msg, const char* file, int line) {
    // Suspend all interrupts immediately so no other UI updates race with BSOD
    arch_disable_interrupts();
    // Make sure UI overlays and cursor exclusions don't interfere
    vga_clear_swap_exclude();
    stop_shell_redirect();
    // Hard reset any prior console/window artifacts
    clearScreen();
    // Ensure the software backbuffer exists; safe no-op if already initialized
    vga_init_double_buffer();
    vga_begin_frame();

    // Choose a soothing dark pastel colour by category
    int br=16, bg=32, bb=96;
    unsigned h = fnv1a_hash(msg, file, line);
    panic_cat_t cat = classify_category(title, msg);
    pick_bg_colour_for_category(cat, h, &br, &bg, &bb);
    // Determine full-screen dimensions from framebuffer
    extern multiboot_info_t *g_mbi;
    int sw = (g_mbi && g_mbi->framebuffer_width) ? (int)g_mbi->framebuffer_width : 640;
    int sh = (g_mbi && g_mbi->framebuffer_height) ? (int)g_mbi->framebuffer_height : 480;
    if (sw <= 0) sw = 640;
    if (sh <= 0) sh = 480;
    // Draw background to backbuffer and also paint directly to framebuffer so it's immediately visible
    drawRect(0, 0, sw, sh, br, bg, bb);
    for (int yy = 0; yy < sh; ++yy) {
        for (int xx = 0; xx < sw; ++xx) {
            vga_drawPixel_fb(xx, yy, br, bg, bb);
        }
    }

    // Title and summary
    drawTextAt(24, 24, ":( EYN-OS encountered a critical error.", 255, 255, 255);
    if (title && *title) {
        drawTextAt(24, 24 + 18, title, 255, 255, 255);
    }

    // Diagnostic lines (build text without relying on full snprintf formatting)
    char buf[256];
    if (msg && *msg) {
        // Reason
        // "Reason: " + msg
        int bi = 0; const char* p = "Reason: "; while (*p && bi < 255) buf[bi++] = *p++;
        p = msg; while (*p && bi < 255) buf[bi++] = *p++;
        buf[bi] = '\0';
        drawTextAt(24, 24 + 44, buf, 255, 255, 255);
    }
    if (file && *file) {
        // Location: file:line
        int bi = 0; const char* p = "Location: "; while (*p && bi < 255) buf[bi++] = *p++;
        p = file; while (*p && bi < 255) buf[bi++] = *p++;
        if (bi < 254) buf[bi++]=':';
        char* ls = int_to_string(line);
        p = ls; while (*p && bi < 255) buf[bi++] = *p++;
        buf[bi] = '\0';
        drawTextAt(24, 24 + 62, buf, 255, 255, 255);
    }

    // Stop code (manual hex formatting) - computed here for later diagnostics section
    char hex[9]; for (int i=0;i<8;i++){ int nib = (h >> ((7-i)*4)) & 0xF; hex[i] = (char)(nib < 10 ? ('0'+nib) : ('A'+(nib-10))); } hex[8]='\0';

    // Right-side plain text diagnostics instead of QR code
    const char* cl = category_label(cat);
    // Place diagnostics beneath the summary block on the left
    int infoX = 24;
    int infoY = 24 + 132; // below Category line with extra spacing
    drawTextAt(infoX, infoY, "Diagnostics:", 255, 255, 255); infoY += 18;
    // Stop code line
    {
        int bi = 0; const char* p = "Stop code: EYNOS_"; while (*p && bi < 255) buf[bi++] = *p++; p = hex; while (*p && bi < 255) buf[bi++] = *p++; buf[bi] = '\0';
        drawTextAt(infoX, infoY, buf, 255, 255, 255); infoY += 16;
    }
    // Category line
    {
        int bi = 0; const char* p = "Category: "; while (*p && bi < 255) buf[bi++] = *p++;
        const char* pcat = cl; while (*pcat && bi < 255) buf[bi++] = *pcat++;
        buf[bi] = '\0';
        drawTextAt(infoX, infoY, buf, 255, 255, 255); infoY += 16;
    }
    // Source file line (shortened to last path segment)
    if (file && *file) {
        const char* last = file; for (const char* s = file; *s; ++s) { if (*s=='/' || *s=='\\') last = s+1; }
        int bi = 0; const char* p = "Source: "; while (*p && bi < 255) buf[bi++] = *p++;
        p = last; while (*p && bi < 255) buf[bi++] = *p++;
        if (bi < 254) { buf[bi++]=':'; }
        char* ls = int_to_string(line);
        p = ls; while (*p && bi < 255) buf[bi++] = *p++;
        buf[bi] = '\0';
        drawTextAt(infoX, infoY, buf, 255, 255, 255); infoY += 16;
    }

    // Instruction: power button
    drawTextAt(24, sh - 66, "Please press the power button to turn off the machine.", 255, 255, 255);
    drawTextAt(24, sh - 48, "A backtrace has been written to the serial log (COM1).", 255, 255, 255);
    drawTextAt(24, sh - 30, "See docs/stop-codes.md to interpret stop codes.", 255, 255, 255);

    // Logo omitted
    // Make sure everything is considered dirty and pushed
    vga_mark_dirty_rect(0, 0, sw, sh);
    // Try to align with vblank to avoid tearing (best-effort)
    vga_wait_vblank();
    vga_swap_buffers();
    // And force-copy the entire backbuffer to framebuffer to fully override any UI remnants
    vga_blit_backbuffer_region_to_fb(0, 0, sw, sh);
}

// Minimal backtrace to serial only (avoid printf to keep BSOD pristine)
static void backtrace_to_serial(void) {
    uint32_t ebp;
    __asm__ volatile ("movl %%ebp, %0" : "=r"(ebp));
    serial_write(SERIAL_COM1, "Backtrace:\n", 11);
    for (int i = 0; i < 16 && ebp; ++i) {
        // Be defensive: a corrupted EBP chain can otherwise fault while panicking.
        if ((ebp & 0x3) != 0) break;           // must be 4-byte aligned
        if (ebp < 0x1000) break;               // never dereference near-null

        uint32_t* frame = (uint32_t*)ebp;
        uint32_t next_ebp = frame[0];
        uint32_t ret = frame[1];
        // Format:  "  #nn 0xXXXXXXXX\n" (avoid relying on snprintf formatting)
        char linebuf[32];
        int pos = 0;
        linebuf[pos++] = ' ';
        linebuf[pos++] = ' ';
        linebuf[pos++] = '#';
        linebuf[pos++] = (char)('0' + ((i / 10) % 10));
        linebuf[pos++] = (char)('0' + (i % 10));
        linebuf[pos++] = ' ';
        linebuf[pos++] = '0';
        linebuf[pos++] = 'x';
        for (int n = 7; n >= 0; --n) {
            uint32_t nib = (ret >> (n * 4)) & 0xFu;
            linebuf[pos++] = (char)((nib < 10) ? ('0' + nib) : ('A' + (nib - 10)));
        }
        linebuf[pos++] = '\n';
        serial_write(SERIAL_COM1, linebuf, pos);

        // EBP should monotonically increase as we unwind (stack grows down).
        if (next_ebp <= ebp) break;
        // Prevent pathological jumps/loops.
        if ((next_ebp - ebp) > 0x10000) break;
        ebp = next_ebp;
    }
}

void panic(const char* msg, const char* file, int line) {
    if (g_panic_in_progress) { halt_forever(); }
    g_panic_in_progress = 1;
    // Render BSOD first
    render_bsod("KERNEL PANIC", msg, file, line);
    // Mirror to serial with details
    serial_write(SERIAL_COM1, "\nKERNEL PANIC: ", 15);
    if (msg) serial_write(SERIAL_COM1, msg, (int)strlen(msg));
    serial_write(SERIAL_COM1, "\n", 1);
    if (file) {
        char loc[256];
        snprintf(loc, sizeof(loc), "At %s:%d\n", file, line);
        serial_write(SERIAL_COM1, loc, (int)strlen(loc));
    }
    backtrace_to_serial();
    halt_forever();
}

void panicf(const char* file, int line, const char* fmt, ...) {
    if (g_panic_in_progress) { halt_forever(); }
    g_panic_in_progress = 1;
    // Format the message once
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int pos = 0;
    const char* p = fmt;
    while (*p && pos < (int)sizeof(buf) - 1) {
        if (*p != '%') {
            buf[pos++] = *p++;
            continue;
        }

        // '%' sequence
        ++p;
        if (*p == '%') {
            buf[pos++] = '%';
            ++p;
            continue;
        }

        // Optional zero-padding and width (supports %08X, %02d)
        char pad = ' ';
        int width = 0;
        if (*p == '0') {
            pad = '0';
            ++p;
        }
        while (*p >= '0' && *p <= '9') {
            width = (width * 10) + (int)(*p - '0');
            ++p;
            if (width > 32) { width = 32; break; }
        }

        char spec = *p ? *p++ : '\0';

        if (spec == 's') {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            while (*s && pos < (int)sizeof(buf) - 1) buf[pos++] = *s++;
            continue;
        }

        if (spec == 'c') {
            buf[pos++] = (char)va_arg(ap, int);
            continue;
        }

        if (spec == 'd' || spec == 'u' || spec == 'x' || spec == 'X' || spec == 'p') {
            uint32_t v;
            int base = (spec == 'x' || spec == 'X' || spec == 'p') ? 16 : 10;
            int upper = (spec == 'X' || spec == 'p');

            if (spec == 'd') {
                int iv = va_arg(ap, int);
                if (iv < 0) {
                    if (pos < (int)sizeof(buf) - 1) buf[pos++] = '-';
                    v = (uint32_t)(-iv);
                } else {
                    v = (uint32_t)iv;
                }
            } else if (spec == 'p') {
                v = (uint32_t)va_arg(ap, void*);
                if (width == 0) { width = 8; pad = '0'; }
            } else {
                v = va_arg(ap, unsigned);
            }

            char tmp[32];
            int ti = 0;
            if (v == 0) {
                tmp[ti++] = '0';
            } else {
                while (v && ti < (int)sizeof(tmp)) {
                    uint32_t digit = (base == 16) ? (v & 0xFu) : (v % 10u);
                    v = (base == 16) ? (v >> 4) : (v / 10u);
                    if (digit < 10) tmp[ti++] = (char)('0' + digit);
                    else tmp[ti++] = (char)((upper ? 'A' : 'a') + (digit - 10));
                }
            }

            while (ti < width && pos < (int)sizeof(buf) - 1) {
                buf[pos++] = pad;
                --width;
            }

            while (ti-- > 0 && pos < (int)sizeof(buf) - 1) {
                buf[pos++] = tmp[ti];
            }
            continue;
        }

        // Unknown specifier: print literally.
        if (pos < (int)sizeof(buf) - 1) buf[pos++] = '%';
        if (pos < (int)sizeof(buf) - 1 && spec) buf[pos++] = spec;
    }
    buf[pos] = '\0';
    va_end(ap);

    render_bsod("KERNEL PANIC", buf, file, line);
    // Serial mirror
    serial_write(SERIAL_COM1, "\nKERNEL PANIC: ", 15);
    serial_write(SERIAL_COM1, buf, (int)strlen(buf));
    serial_write(SERIAL_COM1, "\n", 1);
    if (file) {
        char loc[256];
        snprintf(loc, sizeof(loc), "At %s:%d\n", file, line);
        serial_write(SERIAL_COM1, loc, (int)strlen(loc));
    }
    backtrace_to_serial();
    halt_forever();
}

void assert_fail(const char* expr, const char* file, int line) {
    render_bsod("ASSERTION FAILED", expr, file, line);
    serial_write(SERIAL_COM1, "Assertion failed: ", 18);
    if (expr) serial_write(SERIAL_COM1, expr, (int)strlen(expr));
    serial_write(SERIAL_COM1, "\n", 1);
    if (file) {
        char loc[256];
        snprintf(loc, sizeof(loc), "At %s:%d\n", file, line);
        serial_write(SERIAL_COM1, loc, (int)strlen(loc));
    }
    backtrace_to_serial();
    halt_forever();
}
