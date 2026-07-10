/*
 * x11.c -- X11/Xlib compatibility layer for EYN-OS.
 *
 * Translates standard Xlib function calls into EYN-OS native GUI syscalls.
 * All rendering is done in a userland software framebuffer (RGB565) and
 * pushed to the screen via gui_blit_rgb565 on XFlush/XSync.
 *
 * Architecture constraints:
 *   - Single display, single top-level window (EYN-OS limitation).
 *   - Max framebuffer 320x200 (kernel gui_blit_rgb565 limit).
 *   - Up to 8 GCs, 32-entry event queue.
 *   - No pixmaps, cursors, fonts, or extensions.
 *   - Events translated from EYN-OS gui_event_t → XEvent.
 *
 * SECURITY-INVARIANT: All pixel coordinates are clipped to the framebuffer
 * bounds before writing.  No out-of-bounds framebuffer access is possible
 * regardless of client-supplied coordinates.
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <eynos_syscall.h>
#include <gui.h>
#include <string.h>
#include <stdlib.h>

/* ================================================================== */
/*  Constants                                                          */
/* ================================================================== */

/*
 * SECURITY-INVARIANT: Maximum framebuffer dimensions.
 *
 * Why: The EYN-OS kernel gui_blit_rgb565 syscall copies at most 320x200
 * pixels from the userland buffer into the kernel-side blit surface.
 * Exceeding these dimensions causes the syscall to reject the blit.
 *
 * Invariant: All framebuffer pixel accesses are bounds-checked against
 * these dimensions.  fb_w and fb_h are capped at creation time.
 *
 * Breakage if changed:
 *   - Increasing beyond 320x200: kernel rejects the blit; the window
 *     goes blank.  No memory safety issue (buffer is heap-allocated to
 *     the actual capped size).
 *   - Decreasing: smaller max window; purely cosmetic restriction.
 *
 * ABI-sensitive: No (internal to the shim; not visible to X11 programs).
 */
#define X11_MAX_FB_W    320
#define X11_MAX_FB_H    200

/*
 * Maximum number of GCs that can be simultaneously allocated.
 *
 * Invariant: GC pool index is bounds-checked; XCreateGC returns NULL if
 * the pool is exhausted.
 *
 * Breakage if changed:
 *   - Increasing: more per-GC memory consumed (trivial per GC).
 *   - Decreasing: programs that create many GCs will get NULL back.
 */
#define X11_MAX_GCS     8

/*
 * Internal event queue capacity (ring buffer).
 *
 * Invariant: Head/tail indices are masked to this size.  Must be a
 * power of two.
 */
#define X11_EVQ_SIZE    32
#define X11_EVQ_MASK    (X11_EVQ_SIZE - 1)

/* ================================================================== */
/*  Internal state                                                     */
/* ================================================================== */

/* Per-GC internal storage */
typedef struct {
    int           used;
    struct _XGC   gc;   /* Exposed GC struct including XGCValues */
} x11_gc_slot_t;

/*
 * Master state for the single X11 display/window.
 * Allocated once in XOpenDisplay; freed in XCloseDisplay.
 */
typedef struct {
    int              initialized;

    /* EYN-OS GUI handle for the floating window */
    int              gui_handle;
    int              mapped;

    /* Window geometry (capped to X11_MAX_FB_W x X11_MAX_FB_H) */
    int              win_w, win_h;
    unsigned long    bg_colour;      /* Background pixel for XClearWindow */
    unsigned long    border_colour;
    long             event_mask;

    /* Software framebuffer (RGB565LE, row-major) */
    uint16_t        *fb;
    int              fb_dirty;      /* Non-zero when fb has new content */

    /* GC pool */
    x11_gc_slot_t    gcs[X11_MAX_GCS];

    /* Event queue (ring buffer) */
    XEvent           evq[X11_EVQ_SIZE];
    int              evq_head;
    int              evq_tail;

    /* Previous mouse button state (for press/release edge detection) */
    unsigned int     prev_buttons;

    /* Synthetic events pending */
    int              expose_pending;

    /* Serial counter (monotonically increasing) */
    unsigned long    serial;

    /* X11 structs returned to the user */
    Display          display;
    Screen           screen;
    Visual           visual;
    struct _XGC      default_gc;

    /* Window XID (always 1; root is 0) */
    Window           win_id;
    Window           root_id;
} x11_state_t;

static x11_state_t *g_x11 = NULL;

/* ================================================================== */
/*  RGB888 ↔ RGB565 conversion                                        */
/* ================================================================== */

/*
 * Convert a 24-bit RGB pixel (0x00RRGGBB) to RGB565LE.
 *
 * Representation:
 *   Input:  bits [23:16] = R, [15:8] = G, [7:0] = B
 *   Output: bits [15:11] = R>>3, [10:5] = G>>2, [4:0] = B>>3
 */
static uint16_t rgb_to_565(unsigned long pixel) {
    unsigned int r = (pixel >> 16) & 0xFF;
    unsigned int g = (pixel >>  8) & 0xFF;
    unsigned int b =  pixel        & 0xFF;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* ================================================================== */
/*  Framebuffer drawing primitives (all bounds-checked)                */
/* ================================================================== */

static inline void fb_put(int x, int y, uint16_t c) {
    if (x >= 0 && x < g_x11->win_w && y >= 0 && y < g_x11->win_h) {
        g_x11->fb[y * g_x11->win_w + x] = c;
        g_x11->fb_dirty = 1;
    }
}

static void fb_fill_rect(int x, int y, int w, int h, uint16_t c) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w) > g_x11->win_w ? g_x11->win_w : (x + w);
    int y1 = (y + h) > g_x11->win_h ? g_x11->win_h : (y + h);
    for (int j = y0; j < y1; j++) {
        uint16_t *row = &g_x11->fb[j * g_x11->win_w];
        for (int i = x0; i < x1; i++)
            row[i] = c;
    }
    if (x0 < x1 && y0 < y1)
        g_x11->fb_dirty = 1;
}

static void fb_outline_rect(int x, int y, int w, int h, uint16_t c) {
    /* Top and bottom edges */
    for (int i = x; i < x + w; i++) {
        fb_put(i, y, c);
        fb_put(i, y + h - 1, c);
    }
    /* Left and right edges */
    for (int j = y; j < y + h; j++) {
        fb_put(x, j, c);
        fb_put(x + w - 1, j, c);
    }
}

/*
 * Bresenham line drawing.
 */
static void fb_line(int x0, int y0, int x1, int y1, uint16_t c) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int sx = dx > 0 ? 1 : -1;
    int sy = dy > 0 ? 1 : -1;
    dx = dx < 0 ? -dx : dx;
    dy = dy < 0 ? -dy : dy;

    if (dx >= dy) {
        int err = dx / 2;
        int y = y0;
        for (int x = x0; ; x += sx) {
            fb_put(x, y, c);
            if (x == x1) break;
            err -= dy;
            if (err < 0) { y += sy; err += dx; }
        }
    } else {
        int err = dy / 2;
        int x = x0;
        for (int y = y0; ; y += sy) {
            fb_put(x, y, c);
            if (y == y1) break;
            err -= dx;
            if (err < 0) { x += sx; err += dy; }
        }
    }
}

/*
 * Filled ellipse using the scanline method.
 *
 * For each row within the bounding box, compute the horizontal span
 * of the ellipse and fill it.  Uses 64-bit intermediate products to
 * avoid overflow for radii up to ~32000.
 *
 * Angles are ignored: if angle1==0 and angle2==360*64 (full circle)
 * or if the caller has already validated, draw the full ellipse.
 * Partial arcs are approximated by drawing the full ellipse.
 */
static void fb_fill_ellipse(int cx, int cy, int rx, int ry, uint16_t c) {
    if (rx <= 0 || ry <= 0) return;
    for (int dy = -ry; dy <= ry; dy++) {
        long long yy = (long long)dy * dy;
        long long ry2 = (long long)ry * ry;
        if (yy > ry2) continue;
        long long rx2 = (long long)rx * rx;
        long long x_sq = rx2 * (ry2 - yy) / ry2;
        int x_range = 0;
        while ((long long)(x_range + 1) * (x_range + 1) <= x_sq)
            x_range++;
        int py = cy + dy;
        int left  = cx - x_range;
        int right = cx + x_range;
        /* Clip and fill the scanline */
        if (py >= 0 && py < g_x11->win_h) {
            int l = left  < 0 ? 0 : left;
            int r = right >= g_x11->win_w ? g_x11->win_w - 1 : right;
            uint16_t *row = &g_x11->fb[py * g_x11->win_w];
            for (int x = l; x <= r; x++)
                row[x] = c;
        }
    }
    g_x11->fb_dirty = 1;
}

/*
 * Ellipse outline using the midpoint ellipse algorithm.
 *
 * Plots the four symmetric quadrants of the ellipse boundary.
 */
static void fb_draw_ellipse(int cx, int cy, int rx, int ry, uint16_t c) {
    if (rx <= 0 || ry <= 0) return;

    long long rx2 = (long long)rx * rx;
    long long ry2 = (long long)ry * ry;
    long long two_rx2 = 2 * rx2;
    long long two_ry2 = 2 * ry2;

    int x = 0, y = ry;
    long long px = 0;
    long long py = two_rx2 * y;

    /* Initial four points */
    fb_put(cx, cy + y, c);
    fb_put(cx, cy - y, c);

    /* Region 1: dy/dx < 1 */
    long long p1 = ry2 - rx2 * ry + rx2 / 4;
    while (px < py) {
        x++;
        px += two_ry2;
        if (p1 < 0) {
            p1 += ry2 + px;
        } else {
            y--;
            py -= two_rx2;
            p1 += ry2 + px - py;
        }
        fb_put(cx + x, cy + y, c);
        fb_put(cx - x, cy + y, c);
        fb_put(cx + x, cy - y, c);
        fb_put(cx - x, cy - y, c);
    }

    /* Region 2: dy/dx >= 1 */
    long long p2 = ry2 * ((long long)(2 * x + 1) * (2 * x + 1)) / 4
                 + rx2 * ((long long)(y - 1) * (y - 1))
                 - rx2 * ry2;
    while (y > 0) {
        y--;
        py -= two_rx2;
        if (p2 > 0) {
            p2 += rx2 - py;
        } else {
            x++;
            px += two_ry2;
            p2 += rx2 - py + px;
        }
        fb_put(cx + x, cy + y, c);
        fb_put(cx - x, cy + y, c);
        fb_put(cx + x, cy - y, c);
        fb_put(cx - x, cy - y, c);
    }
}

/*
 * Simple 8x8 bitmap font for XDrawString.
 * Covers printable ASCII 0x20..0x7E.  Each glyph is 8 rows of 8 bits
 * (MSB-left).  This is a minimal font; real X11 font handling would
 * need server-side font catalogs.
 */
static const unsigned char font8x8_basic[95][8] = {
    /* 0x20 ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x21 '!' */ {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    /* 0x22 '"' */ {0x6C,0x6C,0x24,0x00,0x00,0x00,0x00,0x00},
    /* 0x23 '#' */ {0x24,0x7E,0x24,0x24,0x7E,0x24,0x00,0x00},
    /* 0x24 '$' */ {0x18,0x3E,0x58,0x3C,0x1A,0x7C,0x18,0x00},
    /* 0x25 '%' */ {0x62,0x64,0x08,0x10,0x26,0x46,0x00,0x00},
    /* 0x26 '&' */ {0x30,0x48,0x30,0x56,0x48,0x34,0x00,0x00},
    /* 0x27 ''' */ {0x18,0x18,0x08,0x00,0x00,0x00,0x00,0x00},
    /* 0x28 '(' */ {0x08,0x10,0x20,0x20,0x20,0x10,0x08,0x00},
    /* 0x29 ')' */ {0x20,0x10,0x08,0x08,0x08,0x10,0x20,0x00},
    /* 0x2A '*' */ {0x00,0x24,0x18,0x7E,0x18,0x24,0x00,0x00},
    /* 0x2B '+' */ {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    /* 0x2C ',' */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x10},
    /* 0x2D '-' */ {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    /* 0x2E '.' */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
    /* 0x2F '/' */ {0x02,0x04,0x08,0x10,0x20,0x40,0x00,0x00},
    /* 0x30 '0' */ {0x3C,0x46,0x4A,0x52,0x62,0x3C,0x00,0x00},
    /* 0x31 '1' */ {0x18,0x38,0x18,0x18,0x18,0x3C,0x00,0x00},
    /* 0x32 '2' */ {0x3C,0x42,0x02,0x1C,0x20,0x7E,0x00,0x00},
    /* 0x33 '3' */ {0x3C,0x42,0x0C,0x02,0x42,0x3C,0x00,0x00},
    /* 0x34 '4' */ {0x08,0x18,0x28,0x48,0x7E,0x08,0x00,0x00},
    /* 0x35 '5' */ {0x7E,0x40,0x7C,0x02,0x42,0x3C,0x00,0x00},
    /* 0x36 '6' */ {0x1C,0x20,0x40,0x7C,0x42,0x3C,0x00,0x00},
    /* 0x37 '7' */ {0x7E,0x02,0x04,0x08,0x10,0x10,0x00,0x00},
    /* 0x38 '8' */ {0x3C,0x42,0x3C,0x42,0x42,0x3C,0x00,0x00},
    /* 0x39 '9' */ {0x3C,0x42,0x3E,0x02,0x04,0x38,0x00,0x00},
    /* 0x3A ':' */ {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00},
    /* 0x3B ';' */ {0x00,0x18,0x18,0x00,0x18,0x18,0x10,0x00},
    /* 0x3C '<' */ {0x04,0x08,0x10,0x20,0x10,0x08,0x04,0x00},
    /* 0x3D '=' */ {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
    /* 0x3E '>' */ {0x20,0x10,0x08,0x04,0x08,0x10,0x20,0x00},
    /* 0x3F '?' */ {0x3C,0x42,0x04,0x08,0x08,0x00,0x08,0x00},
    /* 0x40 '@' */ {0x3C,0x42,0x4E,0x52,0x4E,0x40,0x3C,0x00},
    /* 0x41 'A' */ {0x18,0x24,0x42,0x7E,0x42,0x42,0x00,0x00},
    /* 0x42 'B' */ {0x7C,0x42,0x7C,0x42,0x42,0x7C,0x00,0x00},
    /* 0x43 'C' */ {0x3C,0x42,0x40,0x40,0x42,0x3C,0x00,0x00},
    /* 0x44 'D' */ {0x78,0x44,0x42,0x42,0x44,0x78,0x00,0x00},
    /* 0x45 'E' */ {0x7E,0x40,0x7C,0x40,0x40,0x7E,0x00,0x00},
    /* 0x46 'F' */ {0x7E,0x40,0x7C,0x40,0x40,0x40,0x00,0x00},
    /* 0x47 'G' */ {0x3C,0x42,0x40,0x4E,0x42,0x3C,0x00,0x00},
    /* 0x48 'H' */ {0x42,0x42,0x7E,0x42,0x42,0x42,0x00,0x00},
    /* 0x49 'I' */ {0x3C,0x18,0x18,0x18,0x18,0x3C,0x00,0x00},
    /* 0x4A 'J' */ {0x1E,0x04,0x04,0x04,0x44,0x38,0x00,0x00},
    /* 0x4B 'K' */ {0x44,0x48,0x50,0x60,0x50,0x48,0x44,0x00},
    /* 0x4C 'L' */ {0x40,0x40,0x40,0x40,0x40,0x7E,0x00,0x00},
    /* 0x4D 'M' */ {0x42,0x66,0x5A,0x42,0x42,0x42,0x00,0x00},
    /* 0x4E 'N' */ {0x42,0x62,0x52,0x4A,0x46,0x42,0x00,0x00},
    /* 0x4F 'O' */ {0x3C,0x42,0x42,0x42,0x42,0x3C,0x00,0x00},
    /* 0x50 'P' */ {0x7C,0x42,0x42,0x7C,0x40,0x40,0x00,0x00},
    /* 0x51 'Q' */ {0x3C,0x42,0x42,0x4A,0x44,0x3A,0x00,0x00},
    /* 0x52 'R' */ {0x7C,0x42,0x42,0x7C,0x48,0x44,0x00,0x00},
    /* 0x53 'S' */ {0x3C,0x40,0x3C,0x02,0x42,0x3C,0x00,0x00},
    /* 0x54 'T' */ {0x7E,0x18,0x18,0x18,0x18,0x18,0x00,0x00},
    /* 0x55 'U' */ {0x42,0x42,0x42,0x42,0x42,0x3C,0x00,0x00},
    /* 0x56 'V' */ {0x42,0x42,0x42,0x24,0x24,0x18,0x00,0x00},
    /* 0x57 'W' */ {0x42,0x42,0x42,0x5A,0x66,0x42,0x00,0x00},
    /* 0x58 'X' */ {0x42,0x24,0x18,0x18,0x24,0x42,0x00,0x00},
    /* 0x59 'Y' */ {0x42,0x42,0x24,0x18,0x18,0x18,0x00,0x00},
    /* 0x5A 'Z' */ {0x7E,0x04,0x08,0x10,0x20,0x7E,0x00,0x00},
    /* 0x5B '[' */ {0x38,0x20,0x20,0x20,0x20,0x20,0x38,0x00},
    /* 0x5C '\' */ {0x40,0x20,0x10,0x08,0x04,0x02,0x00,0x00},
    /* 0x5D ']' */ {0x1C,0x04,0x04,0x04,0x04,0x04,0x1C,0x00},
    /* 0x5E '^' */ {0x10,0x28,0x44,0x00,0x00,0x00,0x00,0x00},
    /* 0x5F '_' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00},
    /* 0x60 '`' */ {0x20,0x10,0x08,0x00,0x00,0x00,0x00,0x00},
    /* 0x61 'a' */ {0x00,0x00,0x3C,0x02,0x3E,0x42,0x3E,0x00},
    /* 0x62 'b' */ {0x40,0x40,0x5C,0x62,0x42,0x62,0x5C,0x00},
    /* 0x63 'c' */ {0x00,0x00,0x3C,0x42,0x40,0x42,0x3C,0x00},
    /* 0x64 'd' */ {0x02,0x02,0x3A,0x46,0x42,0x46,0x3A,0x00},
    /* 0x65 'e' */ {0x00,0x00,0x3C,0x42,0x7E,0x40,0x3C,0x00},
    /* 0x66 'f' */ {0x0C,0x12,0x10,0x38,0x10,0x10,0x10,0x00},
    /* 0x67 'g' */ {0x00,0x00,0x3A,0x46,0x46,0x3A,0x02,0x3C},
    /* 0x68 'h' */ {0x40,0x40,0x5C,0x62,0x42,0x42,0x42,0x00},
    /* 0x69 'i' */ {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
    /* 0x6A 'j' */ {0x04,0x00,0x0C,0x04,0x04,0x04,0x44,0x38},
    /* 0x6B 'k' */ {0x40,0x40,0x44,0x48,0x50,0x68,0x44,0x00},
    /* 0x6C 'l' */ {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    /* 0x6D 'm' */ {0x00,0x00,0x76,0x49,0x49,0x49,0x49,0x00},
    /* 0x6E 'n' */ {0x00,0x00,0x5C,0x62,0x42,0x42,0x42,0x00},
    /* 0x6F 'o' */ {0x00,0x00,0x3C,0x42,0x42,0x42,0x3C,0x00},
    /* 0x70 'p' */ {0x00,0x00,0x5C,0x62,0x62,0x5C,0x40,0x40},
    /* 0x71 'q' */ {0x00,0x00,0x3A,0x46,0x46,0x3A,0x02,0x02},
    /* 0x72 'r' */ {0x00,0x00,0x5C,0x62,0x40,0x40,0x40,0x00},
    /* 0x73 's' */ {0x00,0x00,0x3E,0x40,0x3C,0x02,0x7C,0x00},
    /* 0x74 't' */ {0x10,0x10,0x38,0x10,0x10,0x12,0x0C,0x00},
    /* 0x75 'u' */ {0x00,0x00,0x42,0x42,0x42,0x46,0x3A,0x00},
    /* 0x76 'v' */ {0x00,0x00,0x42,0x42,0x42,0x24,0x18,0x00},
    /* 0x77 'w' */ {0x00,0x00,0x41,0x49,0x49,0x49,0x36,0x00},
    /* 0x78 'x' */ {0x00,0x00,0x42,0x24,0x18,0x24,0x42,0x00},
    /* 0x79 'y' */ {0x00,0x00,0x42,0x42,0x46,0x3A,0x02,0x3C},
    /* 0x7A 'z' */ {0x00,0x00,0x7E,0x04,0x18,0x20,0x7E,0x00},
    /* 0x7B '{' */ {0x0C,0x10,0x10,0x20,0x10,0x10,0x0C,0x00},
    /* 0x7C '|' */ {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00},
    /* 0x7D '}' */ {0x30,0x08,0x08,0x04,0x08,0x08,0x30,0x00},
    /* 0x7E '~' */ {0x00,0x32,0x4C,0x00,0x00,0x00,0x00,0x00},
};

static void fb_draw_char_at(int px, int py, char ch, uint16_t fg) {
    if (ch < 0x20 || ch > 0x7E) return;
    const unsigned char *glyph = font8x8_basic[ch - 0x20];
    for (int row = 0; row < 8; row++) {
        unsigned char bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col))
                fb_put(px + col, py + row, fg);
        }
    }
}

static void fb_draw_string(int x, int y, const char *str, int len, uint16_t fg) {
    for (int i = 0; i < len && str[i]; i++) {
        fb_draw_char_at(x + i * 8, y - 7, str[i], fg);
    }
}

/* ================================================================== */
/*  Event queue helpers                                                */
/* ================================================================== */

static int evq_count(void) {
    return (g_x11->evq_tail - g_x11->evq_head + X11_EVQ_SIZE) & X11_EVQ_MASK;
}

static void evq_push(const XEvent *ev) {
    int next = (g_x11->evq_tail + 1) & X11_EVQ_MASK;
    if (next == g_x11->evq_head) {
        /* Queue full -- drop oldest event */
        g_x11->evq_head = (g_x11->evq_head + 1) & X11_EVQ_MASK;
    }
    g_x11->evq[g_x11->evq_tail] = *ev;
    g_x11->evq_tail = next;
}

static int evq_pop(XEvent *ev) {
    if (g_x11->evq_head == g_x11->evq_tail)
        return 0;
    *ev = g_x11->evq[g_x11->evq_head];
    g_x11->evq_head = (g_x11->evq_head + 1) & X11_EVQ_MASK;
    return 1;
}

/*
 * Translate an EYN-OS gui_event_t into one or more XEvents and enqueue.
 */
static void translate_event(const gui_event_t *ge) {
    if (!g_x11) return;
    XEvent ev;
    memset(&ev, 0, sizeof(ev));

    switch (ge->type) {
    case GUI_EVENT_KEY: {
        /* Map EYN-OS key code to X11 keycode + keysym */
        ev.type = KeyPress;
        ev.xkey.serial = ++g_x11->serial;
        ev.xkey.display = &g_x11->display;
        ev.xkey.window = g_x11->win_id;
        ev.xkey.root = g_x11->root_id;
        ev.xkey.time = CurrentTime;
        ev.xkey.same_screen = True;

        int key = ge->a;
        /*
         * EYN-OS key codes: printable ASCII are their ASCII value.
         * Special keys have values >= 0x1000.
         */
        if (key >= 0x20 && key <= 0x7E) {
            /* Printable ASCII → keycode = key, keysym = key */
            ev.xkey.keycode = key;
        } else if (key == '\r' || key == '\n') {
            ev.xkey.keycode = XK_Return;
        } else if (key == '\b' || key == 0x7F) {
            ev.xkey.keycode = XK_BackSpace;
        } else if (key == '\t') {
            ev.xkey.keycode = XK_Tab;
        } else if (key == 0x1B) {
            ev.xkey.keycode = XK_Escape;
        } else if (key == 0x1001) {
            ev.xkey.keycode = XK_Up;
        } else if (key == 0x1002) {
            ev.xkey.keycode = XK_Down;
        } else if (key == 0x1003) {
            ev.xkey.keycode = XK_Left;
        } else if (key == 0x1004) {
            ev.xkey.keycode = XK_Right;
        } else if (key == 0x1005) {
            ev.xkey.keycode = XK_Delete;
        } else if (key == 0x1006) {
            ev.xkey.keycode = XK_Home;
        } else if (key == 0x1007) {
            ev.xkey.keycode = XK_End;
        } else if (key == 0x1008) {
            ev.xkey.keycode = XK_Page_Up;
        } else if (key == 0x1009) {
            ev.xkey.keycode = XK_Page_Down;
        } else if (key >= 0x5001 && key <= 0x500C) {
            ev.xkey.keycode = XK_F1 + (key - 0x5001);
        } else {
            ev.xkey.keycode = key & 0xFF;
        }

        /* Modifier flags in the key code */
        if (key & 0x3000) ev.xkey.state |= ShiftMask;
        if (key & 0x4000) ev.xkey.state |= Mod4Mask;  /* Super */
        if (key & 0x8000) ev.xkey.state |= ControlMask;

        evq_push(&ev);
        break;
    }

    case GUI_EVENT_MOUSE: {
        int x = ge->a;
        int y = ge->b;
        unsigned int buttons = (unsigned int)ge->c;

        /* Check for button press/release edges */
        unsigned int changed = buttons ^ g_x11->prev_buttons;
        if (changed) {
            for (int btn = 0; btn < 3; btn++) {
                unsigned int mask = 1u << btn;
                if (changed & mask) {
                    memset(&ev, 0, sizeof(ev));
                    ev.type = (buttons & mask) ? ButtonPress : ButtonRelease;
                    ev.xbutton.serial = ++g_x11->serial;
                    ev.xbutton.display = &g_x11->display;
                    ev.xbutton.window = g_x11->win_id;
                    ev.xbutton.root = g_x11->root_id;
                    ev.xbutton.time = CurrentTime;
                    ev.xbutton.x = x;
                    ev.xbutton.y = y;
                    ev.xbutton.x_root = x;
                    ev.xbutton.y_root = y;
                    ev.xbutton.button = btn + 1; /* X11: Button1=1 */
                    ev.xbutton.same_screen = True;
                    /* State reflects buttons before this event */
                    if (g_x11->prev_buttons & 0x01) ev.xbutton.state |= Button1Mask;
                    if (g_x11->prev_buttons & 0x02) ev.xbutton.state |= Button3Mask;
                    if (g_x11->prev_buttons & 0x04) ev.xbutton.state |= Button2Mask;
                    evq_push(&ev);
                }
            }
            g_x11->prev_buttons = buttons;
        }

        /* Always generate MotionNotify */
        memset(&ev, 0, sizeof(ev));
        ev.type = MotionNotify;
        ev.xmotion.serial = ++g_x11->serial;
        ev.xmotion.display = &g_x11->display;
        ev.xmotion.window = g_x11->win_id;
        ev.xmotion.root = g_x11->root_id;
        ev.xmotion.time = CurrentTime;
        ev.xmotion.x = x;
        ev.xmotion.y = y;
        ev.xmotion.x_root = x;
        ev.xmotion.y_root = y;
        ev.xmotion.same_screen = True;
        if (buttons & 0x01) ev.xmotion.state |= Button1Mask;
        if (buttons & 0x02) ev.xmotion.state |= Button3Mask;
        if (buttons & 0x04) ev.xmotion.state |= Button2Mask;
        evq_push(&ev);
        break;
    }

    case GUI_EVENT_CLOSE: {
        /* Deliver as ClientMessage with WM_DELETE_WINDOW convention */
        memset(&ev, 0, sizeof(ev));
        ev.type = ClientMessage;
        ev.xclient.serial = ++g_x11->serial;
        ev.xclient.display = &g_x11->display;
        ev.xclient.window = g_x11->win_id;
        ev.xclient.message_type = XA_WM_COMMAND;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = 0; /* "WM_DELETE_WINDOW" sentinel */
        evq_push(&ev);
        break;
    }

    default:
        break;
    }
}

/*
 * Poll all pending EYN-OS events and translate them.
 */
static void pump_events(void) {
    if (!g_x11 || g_x11->gui_handle < 0) return;
    gui_event_t ge;
    while (eyn_syscall3(EYN_SYSCALL_GUI_POLL_EVENT, g_x11->gui_handle,
                        &ge, (int)sizeof(ge)) == 1) {
        translate_event(&ge);
    }
}

/*
 * Flush the software framebuffer to the EYN-OS GUI.
 */
static void flush_fb(void) {
    if (!g_x11 || g_x11->gui_handle < 0 || !g_x11->mapped) return;
    if (!g_x11->fb_dirty) return;

    eyn_syscall1(EYN_SYSCALL_GUI_BEGIN, g_x11->gui_handle);

    gui_blit_rgb565_t blit;
    blit.src_w = g_x11->win_w;
    blit.src_h = g_x11->win_h;
    blit.pixels = g_x11->fb;
    blit.dst_w = 0; /* Use content size */
    blit.dst_h = 0;
    eyn_syscall3(EYN_SYSCALL_GUI_BLIT_RGB565, g_x11->gui_handle, &blit, 0);

    eyn_syscall1(EYN_SYSCALL_GUI_PRESENT, g_x11->gui_handle);
    g_x11->fb_dirty = 0;
}

/* ================================================================== */
/*  Xlib function implementations                                     */
/* ================================================================== */

/* ---- Connection -------------------------------------------------- */

Display *XOpenDisplay(const char *display_name) {
    (void)display_name;

    if (g_x11) return &g_x11->display; /* Already open */

    g_x11 = (x11_state_t *)calloc(1, sizeof(x11_state_t));
    if (!g_x11) return NULL;

    /* We don't have a window yet; use reasonable defaults */
    int scr_w = 320;
    int scr_h = 200;

    /* Set up Visual */
    g_x11->visual.visualid = 1;
    g_x11->visual.class_  = TrueColour;
    g_x11->visual.bits_per_rgb = 8;
    g_x11->visual.map_entries = 256;
    g_x11->visual.red_mask   = 0xFF0000;
    g_x11->visual.green_mask = 0x00FF00;
    g_x11->visual.blue_mask  = 0x0000FF;

    /* Root window */
    g_x11->root_id = 0;
    g_x11->win_id  = 1;  /* Will be assigned on XCreateSimpleWindow */

    /* Set up Screen */
    g_x11->screen.display     = &g_x11->display;
    g_x11->screen.root        = g_x11->root_id;
    g_x11->screen.width       = scr_w;
    g_x11->screen.height      = scr_h;
    g_x11->screen.mwidth      = scr_w * 254 / 960; /* ~96 DPI */
    g_x11->screen.mheight     = scr_h * 254 / 960;
    g_x11->screen.ndepths     = 1;
    g_x11->screen.root_depth  = 24;
    g_x11->screen.root_visual = &g_x11->visual;
    g_x11->screen.default_gc  = &g_x11->default_gc;
    g_x11->screen.cmap        = 1;
    g_x11->screen.white_pixel = 0xFFFFFF;
    g_x11->screen.black_pixel = 0x000000;

    /* Default GC */
    memset(&g_x11->default_gc, 0, sizeof(g_x11->default_gc));
    g_x11->default_gc.values.function   = GXcopy;
    g_x11->default_gc.values.foreground = 0x000000;
    g_x11->default_gc.values.background = 0xFFFFFF;
    g_x11->default_gc.values.line_width = 0;
    g_x11->default_gc.values.fill_style = FillSolid;
    g_x11->default_gc._id = -1;

    /* Set up Display */
    g_x11->display.fd = 0;
    g_x11->display.nscreens = 1;
    g_x11->display.default_screen = 0;
    g_x11->display.screens = &g_x11->screen;
    g_x11->display._x11_priv = g_x11;

    g_x11->gui_handle = -1;
    g_x11->initialized = 1;

    return &g_x11->display;
}

int XCloseDisplay(Display *display) {
    (void)display;
    if (!g_x11) return 0;
    /* Note: gui_create windows are cleaned up when the process exits */
    g_x11->initialized = 0;
    g_x11 = NULL;  /* Don't free -- bump allocator, free is no-op */
    return 0;
}

/* ---- Window management ------------------------------------------- */

Window XCreateSimpleWindow(Display *display, Window parent,
                           int x, int y,
                           unsigned int width, unsigned int height,
                           unsigned int border_width,
                           unsigned long border, unsigned long background) {
    (void)display; (void)parent; (void)x; (void)y; (void)border_width;

    if (!g_x11) return None;
    if (g_x11->gui_handle >= 0) return g_x11->win_id; /* Already created */

    /* Cap dimensions to the framebuffer limit */
    int w = (int)width;
    int h = (int)height;
    if (w > X11_MAX_FB_W) w = X11_MAX_FB_W;
    if (h > X11_MAX_FB_H) h = X11_MAX_FB_H;
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    g_x11->win_w = w;
    g_x11->win_h = h;
    g_x11->bg_colour = background;
    g_x11->border_colour = border;

    /* Allocate the software framebuffer */
    g_x11->fb = (uint16_t *)calloc(w * h, sizeof(uint16_t));
    if (!g_x11->fb) return None;

    /* Fill with background colour */
    uint16_t bg565 = rgb_to_565(background);
    for (int i = 0; i < w * h; i++)
        g_x11->fb[i] = bg565;
    g_x11->fb_dirty = 1;

    /* Create the EYN-OS GUI floating window */
    g_x11->gui_handle = eyn_syscall3_pii(EYN_SYSCALL_GUI_CREATE,
                                         "X11", 0, 0);
    if (g_x11->gui_handle < 0) return None;

    /* Enable continuous redraw for smooth animation */
    eyn_syscall3(EYN_SYSCALL_GUI_SET_CONTINUOUS_REDRAW,
                 g_x11->gui_handle, (const void *)1, 0);

    return g_x11->win_id;
}

Window XCreateWindow(Display *display, Window parent,
                     int x, int y,
                     unsigned int width, unsigned int height,
                     unsigned int border_width,
                     int depth, unsigned int class_,
                     Visual *visual,
                     unsigned long valuemask,
                     XSetWindowAttributes *attributes) {
    (void)depth; (void)class_; (void)visual;

    unsigned long bg = 0x000000;
    unsigned long border = 0x000000;
    if (attributes && (valuemask & CWBackPixel))
        bg = attributes->background_pixel;
    if (attributes && (valuemask & CWBorderPixel))
        border = attributes->border_pixel;
    if (attributes && (valuemask & CWEventMask))
        g_x11->event_mask = attributes->event_mask;

    return XCreateSimpleWindow(display, parent, x, y, width, height,
                               border_width, border, bg);
}

int XDestroyWindow(Display *display, Window w) {
    (void)display; (void)w;
    /* Window cleanup happens on process exit */
    return 0;
}

int XMapWindow(Display *display, Window w) {
    (void)display; (void)w;
    if (!g_x11) return 0;

    g_x11->mapped = 1;
    g_x11->expose_pending = 1; /* Will deliver Expose on next event poll */

    /* Push initial Expose event */
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = Expose;
    ev.xexpose.serial = ++g_x11->serial;
    ev.xexpose.display = &g_x11->display;
    ev.xexpose.window = g_x11->win_id;
    ev.xexpose.x = 0;
    ev.xexpose.y = 0;
    ev.xexpose.width  = g_x11->win_w;
    ev.xexpose.height = g_x11->win_h;
    ev.xexpose.count  = 0;
    evq_push(&ev);

    /* Also push MapNotify */
    memset(&ev, 0, sizeof(ev));
    ev.type = MapNotify;
    ev.xmap.serial = ++g_x11->serial;
    ev.xmap.display = &g_x11->display;
    ev.xmap.event = g_x11->win_id;
    ev.xmap.window = g_x11->win_id;
    ev.xmap.override_redirect = False;
    evq_push(&ev);

    /* Do initial blit */
    flush_fb();

    return 0;
}

int XUnmapWindow(Display *display, Window w) {
    (void)display; (void)w;
    if (g_x11) g_x11->mapped = 0;
    return 0;
}

int XMapRaised(Display *display, Window w) {
    return XMapWindow(display, w);
}

int XRaiseWindow(Display *display, Window w) {
    (void)display; (void)w;
    return 0;
}

int XLowerWindow(Display *display, Window w) {
    (void)display; (void)w;
    return 0;
}

int XMoveWindow(Display *display, Window w, int x, int y) {
    (void)display; (void)w; (void)x; (void)y;
    return 0;
}

int XResizeWindow(Display *display, Window w,
                  unsigned int width, unsigned int height) {
    (void)display; (void)w; (void)width; (void)height;
    return 0;
}

int XMoveResizeWindow(Display *display, Window w,
                      int x, int y,
                      unsigned int width, unsigned int height) {
    (void)display; (void)w; (void)x; (void)y;
    (void)width; (void)height;
    return 0;
}

/* ---- Window properties ------------------------------------------- */

int XStoreName(Display *display, Window w, const char *name) {
    (void)display; (void)w;
    if (!g_x11 || g_x11->gui_handle < 0) return 0;
    eyn_syscall3(EYN_SYSCALL_GUI_SET_TITLE, g_x11->gui_handle, name, 0);
    return 0;
}

Status XFetchName(Display *display, Window w, char **name_return) {
    (void)display; (void)w;
    if (name_return) *name_return = NULL;
    return 0;
}

int XSetIconName(Display *display, Window w, const char *icon_name) {
    (void)display; (void)w; (void)icon_name;
    return 0;
}

int XChangeProperty(Display *display, Window w,
                    Atom property, Atom type, int format, int mode,
                    const unsigned char *data, int nelements) {
    (void)display; (void)w; (void)property; (void)type;
    (void)format; (void)mode; (void)data; (void)nelements;
    return 0;
}

int XSetWMProtocols(Display *display, Window w,
                    Atom *protocols, int count) {
    (void)display; (void)w; (void)protocols; (void)count;
    return 0;
}

Atom XInternAtom(Display *display, const char *atom_name, Bool only_if_exists) {
    (void)display; (void)only_if_exists;
    /* Return a deterministic hash-based atom for known names */
    if (!atom_name) return None;
    if (strcmp(atom_name, "WM_DELETE_WINDOW") == 0) return 100;
    if (strcmp(atom_name, "WM_PROTOCOLS") == 0) return 101;
    if (strcmp(atom_name, "_NET_WM_NAME") == 0) return 102;
    if (strcmp(atom_name, "UTF8_STRING") == 0) return 103;
    /* Generic: hash the name into a unique-enough Atom */
    unsigned long h = 200;
    while (*atom_name) {
        h = h * 31 + (unsigned char)*atom_name++;
    }
    return (Atom)(h & 0x7FFFFFFF);
}

/* ---- Input selection --------------------------------------------- */

int XSelectInput(Display *display, Window w, long event_mask) {
    (void)display; (void)w;
    if (g_x11) g_x11->event_mask = event_mask;
    return 0;
}

/* ---- GC management ----------------------------------------------- */

GC XCreateGC(Display *display, Drawable d,
             unsigned long valuemask, XGCValues *values) {
    (void)display; (void)d;

    if (!g_x11) return NULL;

    /* Find a free GC slot */
    int idx = -1;
    for (int i = 0; i < X11_MAX_GCS; i++) {
        if (!g_x11->gcs[i].used) { idx = i; break; }
    }
    if (idx < 0) return NULL;

    g_x11->gcs[idx].used = 1;
    struct _XGC *gc = &g_x11->gcs[idx].gc;
    memset(gc, 0, sizeof(*gc));
    gc->_id = idx;

    /* Defaults */
    gc->values.function   = GXcopy;
    gc->values.foreground = 0x000000;
    gc->values.background = 0xFFFFFF;
    gc->values.line_width = 0;
    gc->values.line_style = LineSolid;
    gc->values.cap_style  = CapButt;
    gc->values.join_style = JoinMiter;
    gc->values.fill_style = FillSolid;
    gc->values.arc_mode   = ArcPieSlice;
    gc->values.graphics_exposures = True;

    /* Apply supplied values */
    if (values) {
        if (valuemask & GCFunction)   gc->values.function   = values->function;
        if (valuemask & GCForeground) gc->values.foreground = values->foreground;
        if (valuemask & GCBackground) gc->values.background = values->background;
        if (valuemask & GCLineWidth)  gc->values.line_width = values->line_width;
        if (valuemask & GCLineStyle)  gc->values.line_style = values->line_style;
        if (valuemask & GCCapStyle)   gc->values.cap_style  = values->cap_style;
        if (valuemask & GCJoinStyle)  gc->values.join_style = values->join_style;
        if (valuemask & GCFillStyle)  gc->values.fill_style = values->fill_style;
        if (valuemask & GCArcMode)    gc->values.arc_mode   = values->arc_mode;
    }

    return gc;
}

int XFreeGC(Display *display, GC gc) {
    (void)display;
    if (!g_x11 || !gc) return 0;
    int idx = gc->_id;
    if (idx >= 0 && idx < X11_MAX_GCS)
        g_x11->gcs[idx].used = 0;
    return 0;
}

int XSetForeground(Display *display, GC gc, unsigned long foreground) {
    (void)display;
    if (gc) gc->values.foreground = foreground;
    return 0;
}

int XSetBackground(Display *display, GC gc, unsigned long background) {
    (void)display;
    if (gc) gc->values.background = background;
    return 0;
}

int XSetFunction(Display *display, GC gc, int function) {
    (void)display;
    if (gc) gc->values.function = function;
    return 0;
}

int XSetLineAttributes(Display *display, GC gc,
                       unsigned int line_width, int line_style,
                       int cap_style, int join_style) {
    (void)display;
    if (!gc) return 0;
    gc->values.line_width = (int)line_width;
    gc->values.line_style = line_style;
    gc->values.cap_style  = cap_style;
    gc->values.join_style = join_style;
    return 0;
}

int XSetFillStyle(Display *display, GC gc, int fill_style) {
    (void)display;
    if (gc) gc->values.fill_style = fill_style;
    return 0;
}

int XChangeGC(Display *display, GC gc,
              unsigned long valuemask, XGCValues *values) {
    (void)display;
    if (!gc || !values) return 0;
    if (valuemask & GCFunction)   gc->values.function   = values->function;
    if (valuemask & GCForeground) gc->values.foreground = values->foreground;
    if (valuemask & GCBackground) gc->values.background = values->background;
    if (valuemask & GCLineWidth)  gc->values.line_width = values->line_width;
    if (valuemask & GCLineStyle)  gc->values.line_style = values->line_style;
    if (valuemask & GCCapStyle)   gc->values.cap_style  = values->cap_style;
    if (valuemask & GCJoinStyle)  gc->values.join_style = values->join_style;
    if (valuemask & GCFillStyle)  gc->values.fill_style = values->fill_style;
    if (valuemask & GCArcMode)    gc->values.arc_mode   = values->arc_mode;
    return 0;
}

int XGetGCValues(Display *display, GC gc,
                 unsigned long valuemask, XGCValues *values_return) {
    (void)display; (void)valuemask;
    if (gc && values_return) *values_return = gc->values;
    return 1;
}

GC XCopyGC(Display *display, GC src, unsigned long valuemask, GC dest) {
    (void)display;
    if (src && dest) {
        XChangeGC(display, dest, valuemask, &src->values);
    }
    return dest;
}

/* ---- Drawing primitives ------------------------------------------ */

int XClearWindow(Display *display, Window w) {
    (void)display; (void)w;
    if (!g_x11 || !g_x11->fb) return 0;
    uint16_t bg = rgb_to_565(g_x11->bg_colour);
    for (int i = 0; i < g_x11->win_w * g_x11->win_h; i++)
        g_x11->fb[i] = bg;
    g_x11->fb_dirty = 1;
    return 0;
}

int XClearArea(Display *display, Window w,
               int x, int y,
               unsigned int width, unsigned int height,
               Bool exposures) {
    (void)display; (void)w; (void)exposures;
    if (!g_x11) return 0;
    int cw = (int)width  > 0 ? (int)width  : g_x11->win_w;
    int ch = (int)height > 0 ? (int)height : g_x11->win_h;
    fb_fill_rect(x, y, cw, ch, rgb_to_565(g_x11->bg_colour));
    return 0;
}

int XDrawPoint(Display *display, Drawable d, GC gc, int x, int y) {
    (void)display; (void)d;
    if (!g_x11 || !gc) return 0;
    fb_put(x, y, rgb_to_565(gc->values.foreground));
    return 0;
}

int XDrawPoints(Display *display, Drawable d, GC gc,
                XPoint *points, int npoints, int mode) {
    (void)mode;
    for (int i = 0; i < npoints; i++)
        XDrawPoint(display, d, gc, points[i].x, points[i].y);
    return 0;
}

int XDrawLine(Display *display, Drawable d, GC gc,
              int x1, int y1, int x2, int y2) {
    (void)display; (void)d;
    if (!g_x11 || !gc) return 0;
    fb_line(x1, y1, x2, y2, rgb_to_565(gc->values.foreground));
    return 0;
}

int XDrawLines(Display *display, Drawable d, GC gc,
               XPoint *points, int npoints, int mode) {
    (void)mode;
    if (npoints < 2) return 0;
    for (int i = 0; i < npoints - 1; i++)
        XDrawLine(display, d, gc,
                  points[i].x, points[i].y,
                  points[i+1].x, points[i+1].y);
    return 0;
}

int XDrawSegments(Display *display, Drawable d, GC gc,
                  XSegment *segments, int nsegments) {
    for (int i = 0; i < nsegments; i++)
        XDrawLine(display, d, gc,
                  segments[i].x1, segments[i].y1,
                  segments[i].x2, segments[i].y2);
    return 0;
}

int XDrawRectangle(Display *display, Drawable d, GC gc,
                   int x, int y,
                   unsigned int width, unsigned int height) {
    (void)display; (void)d;
    if (!g_x11 || !gc) return 0;
    fb_outline_rect(x, y, (int)width + 1, (int)height + 1,
                    rgb_to_565(gc->values.foreground));
    return 0;
}

int XFillRectangle(Display *display, Drawable d, GC gc,
                   int x, int y,
                   unsigned int width, unsigned int height) {
    (void)display; (void)d;
    if (!g_x11 || !gc) return 0;
    fb_fill_rect(x, y, (int)width, (int)height,
                 rgb_to_565(gc->values.foreground));
    return 0;
}

int XFillRectangles(Display *display, Drawable d, GC gc,
                    XRectangle *rectangles, int nrectangles) {
    for (int i = 0; i < nrectangles; i++)
        XFillRectangle(display, d, gc,
                       rectangles[i].x, rectangles[i].y,
                       rectangles[i].width, rectangles[i].height);
    return 0;
}

int XDrawArc(Display *display, Drawable d, GC gc,
             int x, int y,
             unsigned int width, unsigned int height,
             int angle1, int angle2) {
    (void)display; (void)d; (void)angle1; (void)angle2;
    if (!g_x11 || !gc) return 0;
    /*
     * X11 arc parameters: bounding box (x, y, width, height).
     * Center = (x + width/2, y + height/2), radii = (width/2, height/2).
     * Angles are in 64ths of a degree; ignored here (full ellipse drawn).
     */
    int rx = (int)width  / 2;
    int ry = (int)height / 2;
    int cx = x + rx;
    int cy = y + ry;
    fb_draw_ellipse(cx, cy, rx, ry, rgb_to_565(gc->values.foreground));
    return 0;
}

int XDrawArcs(Display *display, Drawable d, GC gc,
              XArc *arcs, int narcs) {
    for (int i = 0; i < narcs; i++)
        XDrawArc(display, d, gc,
                 arcs[i].x, arcs[i].y,
                 arcs[i].width, arcs[i].height,
                 arcs[i].angle1, arcs[i].angle2);
    return 0;
}

int XFillArc(Display *display, Drawable d, GC gc,
             int x, int y,
             unsigned int width, unsigned int height,
             int angle1, int angle2) {
    (void)display; (void)d; (void)angle1; (void)angle2;
    if (!g_x11 || !gc) return 0;
    int rx = (int)width  / 2;
    int ry = (int)height / 2;
    int cx = x + rx;
    int cy = y + ry;
    fb_fill_ellipse(cx, cy, rx, ry, rgb_to_565(gc->values.foreground));
    return 0;
}

int XFillArcs(Display *display, Drawable d, GC gc,
              XArc *arcs, int narcs) {
    for (int i = 0; i < narcs; i++)
        XFillArc(display, d, gc,
                 arcs[i].x, arcs[i].y,
                 arcs[i].width, arcs[i].height,
                 arcs[i].angle1, arcs[i].angle2);
    return 0;
}

int XFillPolygon(Display *display, Drawable d, GC gc,
                 XPoint *points, int npoints,
                 int shape, int mode) {
    (void)display; (void)d; (void)shape; (void)mode;
    if (!g_x11 || !gc || npoints < 3) return 0;

    uint16_t colour = rgb_to_565(gc->values.foreground);

    /* Find bounding box */
    int min_y = points[0].y, max_y = points[0].y;
    for (int i = 1; i < npoints; i++) {
        if (points[i].y < min_y) min_y = points[i].y;
        if (points[i].y > max_y) max_y = points[i].y;
    }

    /* Simple scanline fill */
    for (int y = min_y; y <= max_y; y++) {
        /* Find intersection x-coordinates with all edges */
        int xs[64];
        int nx = 0;
        for (int i = 0; i < npoints && nx < 62; i++) {
            int j = (i + 1) % npoints;
            int y0 = points[i].y, y1 = points[j].y;
            int x0 = points[i].x, x1 = points[j].x;
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
                int ix = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
                xs[nx++] = ix;
            }
        }
        /* Sort intersections */
        for (int a = 0; a < nx - 1; a++)
            for (int b = a + 1; b < nx; b++)
                if (xs[a] > xs[b]) { int tmp = xs[a]; xs[a] = xs[b]; xs[b] = tmp; }
        /* Fill between pairs */
        for (int k = 0; k + 1 < nx; k += 2) {
            for (int x = xs[k]; x <= xs[k+1]; x++)
                fb_put(x, y, colour);
        }
    }
    g_x11->fb_dirty = 1;
    return 0;
}

/* ---- Text -------------------------------------------------------- */

int XDrawString(Display *display, Drawable d, GC gc,
                int x, int y, const char *string, int length) {
    (void)display; (void)d;
    if (!g_x11 || !gc || !string) return 0;
    fb_draw_string(x, y, string, length, rgb_to_565(gc->values.foreground));
    return 0;
}

int XDrawImageString(Display *display, Drawable d, GC gc,
                     int x, int y, const char *string, int length) {
    /* ImageString also draws the background behind each character */
    (void)display; (void)d;
    if (!g_x11 || !gc || !string) return 0;
    uint16_t bg = rgb_to_565(gc->values.background);
    fb_fill_rect(x, y - 7, length * 8, 8, bg);
    fb_draw_string(x, y, string, length, rgb_to_565(gc->values.foreground));
    return 0;
}

/* ---- Input ------------------------------------------------------- */

int XLookupString(XKeyEvent *event_struct,
                  char *buffer_return, int bytes_buffer,
                  KeySym *keysym_return,
                  XComposeStatus *status_in_out) {
    (void)status_in_out;
    if (!event_struct) return 0;

    unsigned int keycode = event_struct->keycode;
    KeySym ks = (KeySym)keycode;

    if (keysym_return) *keysym_return = ks;

    /* If it's a printable ASCII character, put it in the buffer */
    if (keycode >= 0x20 && keycode <= 0x7E && buffer_return && bytes_buffer > 0) {
        buffer_return[0] = (char)keycode;
        if (bytes_buffer > 1) buffer_return[1] = '\0';
        return 1;
    }
    if (keycode == XK_Return && buffer_return && bytes_buffer > 0) {
        buffer_return[0] = '\r';
        if (bytes_buffer > 1) buffer_return[1] = '\0';
        return 1;
    }
    if (keycode == XK_BackSpace && buffer_return && bytes_buffer > 0) {
        buffer_return[0] = '\b';
        if (bytes_buffer > 1) buffer_return[1] = '\0';
        return 1;
    }
    if (keycode == XK_Escape && buffer_return && bytes_buffer > 0) {
        buffer_return[0] = 0x1B;
        if (bytes_buffer > 1) buffer_return[1] = '\0';
        return 1;
    }

    /* Non-printable / special key: no character output */
    if (buffer_return && bytes_buffer > 0) buffer_return[0] = '\0';
    return 0;
}

/* ---- Event handling ---------------------------------------------- */

int XNextEvent(Display *display, XEvent *event_return) {
    (void)display;
    if (!g_x11 || !event_return) return 0;

    /* First, try to dequeue from our internal queue */
    if (evq_pop(event_return))
        return 0;

    /* No internal events -- block on EYN-OS GUI event */
    gui_event_t ge;
    while (1) {
        int rc = eyn_syscall3(EYN_SYSCALL_GUI_WAIT_EVENT,
                              g_x11->gui_handle, &ge, (int)sizeof(ge));
        if (rc < 0) {
            /* Error or interrupted -- synthesize a ClientMessage */
            memset(event_return, 0, sizeof(*event_return));
            event_return->type = ClientMessage;
            event_return->xclient.display = &g_x11->display;
            event_return->xclient.window = g_x11->win_id;
            return 0;
        }
        translate_event(&ge);
        if (evq_pop(event_return))
            return 0;
    }
}

int XPeekEvent(Display *display, XEvent *event_return) {
    (void)display;
    if (!g_x11 || !event_return) return 0;

    pump_events();
    if (evq_count() > 0) {
        *event_return = g_x11->evq[g_x11->evq_head];
        return 0;
    }

    /* Block until one arrives */
    gui_event_t ge;
    int rc = eyn_syscall3(EYN_SYSCALL_GUI_WAIT_EVENT,
                          g_x11->gui_handle, &ge, (int)sizeof(ge));
    if (rc >= 0) translate_event(&ge);
    if (evq_count() > 0)
        *event_return = g_x11->evq[g_x11->evq_head];
    return 0;
}

int XPending(Display *display) {
    (void)display;
    if (!g_x11) return 0;
    pump_events();
    return evq_count();
}

int XEventsQueued(Display *display, int mode) {
    (void)mode;
    return XPending(display);
}

Bool XCheckWindowEvent(Display *display, Window w,
                       long event_mask, XEvent *event_return) {
    (void)display; (void)w; (void)event_mask;
    pump_events();
    if (evq_count() > 0)
        return evq_pop(event_return) ? True : False;
    return False;
}

Bool XCheckTypedEvent(Display *display, int event_type,
                      XEvent *event_return) {
    (void)display;
    pump_events();
    /* Linear scan for matching event type */
    int count = evq_count();
    for (int i = 0; i < count; i++) {
        int idx = (g_x11->evq_head + i) & X11_EVQ_MASK;
        if (g_x11->evq[idx].type == event_type) {
            *event_return = g_x11->evq[idx];
            /* Remove from queue by shifting remaining elements */
            for (int j = i; j < count - 1; j++) {
                int from = (g_x11->evq_head + j + 1) & X11_EVQ_MASK;
                int to   = (g_x11->evq_head + j)     & X11_EVQ_MASK;
                g_x11->evq[to] = g_x11->evq[from];
            }
            g_x11->evq_tail = (g_x11->evq_tail - 1 + X11_EVQ_SIZE) & X11_EVQ_MASK;
            return True;
        }
    }
    return False;
}

Bool XCheckTypedWindowEvent(Display *display, Window w,
                            int event_type, XEvent *event_return) {
    (void)w;
    return XCheckTypedEvent(display, event_type, event_return);
}

Bool XCheckMaskEvent(Display *display, long event_mask,
                     XEvent *event_return) {
    (void)event_mask;
    return XCheckWindowEvent(display, None, event_mask, event_return);
}

/* ---- Flush / sync ------------------------------------------------ */

int XFlush(Display *display) {
    (void)display;
    flush_fb();
    return 0;
}

int XSync(Display *display, Bool discard) {
    (void)display;
    flush_fb();
    if (discard && g_x11) {
        g_x11->evq_head = 0;
        g_x11->evq_tail = 0;
    }
    return 0;
}

/* ---- Window queries ---------------------------------------------- */

Status XGetWindowAttributes(Display *display, Window w,
                            XWindowAttributes *attr) {
    (void)display; (void)w;
    if (!g_x11 || !attr) return 0;
    memset(attr, 0, sizeof(*attr));
    attr->x = 0;
    attr->y = 0;
    attr->width  = g_x11->win_w;
    attr->height = g_x11->win_h;
    attr->depth  = 24;
    attr->visual = &g_x11->visual;
    attr->root   = g_x11->root_id;
    attr->class_ = InputOutput;
    attr->map_state = g_x11->mapped ? IsViewable : IsUnmapped;
    attr->your_event_mask = g_x11->event_mask;
    attr->screen = &g_x11->screen;
    return 1;
}

Status XGetGeometry(Display *display, Drawable d,
                    Window *root_return,
                    int *x_return, int *y_return,
                    unsigned int *width_return, unsigned int *height_return,
                    unsigned int *border_width_return,
                    unsigned int *depth_return) {
    (void)display; (void)d;
    if (!g_x11) return 0;
    if (root_return) *root_return = g_x11->root_id;
    if (x_return) *x_return = 0;
    if (y_return) *y_return = 0;
    if (width_return)  *width_return  = (unsigned int)g_x11->win_w;
    if (height_return) *height_return = (unsigned int)g_x11->win_h;
    if (border_width_return) *border_width_return = 0;
    if (depth_return) *depth_return = 24;
    return 1;
}

Bool XQueryPointer(Display *display, Window w,
                   Window *root_return, Window *child_return,
                   int *root_x_return, int *root_y_return,
                   int *win_x_return, int *win_y_return,
                   unsigned int *mask_return) {
    (void)display; (void)w;
    /* We don't have a direct pointer query; return last known position */
    if (root_return)  *root_return  = g_x11 ? g_x11->root_id : 0;
    if (child_return) *child_return = None;
    if (root_x_return) *root_x_return = 0;
    if (root_y_return) *root_y_return = 0;
    if (win_x_return) *win_x_return = 0;
    if (win_y_return) *win_y_return = 0;
    if (mask_return) *mask_return = 0;
    return True;
}

/* ---- Colour ------------------------------------------------------- */

Status XAllocColour(Display *display, Colourmap colourmap,
                   XColour *screen_in_out) {
    (void)display; (void)colourmap;
    if (!screen_in_out) return 0;
    /* TrueColour: pack the 16-bit channel values into a 24-bit pixel */
    unsigned long r = (screen_in_out->red   >> 8) & 0xFF;
    unsigned long g = (screen_in_out->green >> 8) & 0xFF;
    unsigned long b = (screen_in_out->blue  >> 8) & 0xFF;
    screen_in_out->pixel = (r << 16) | (g << 8) | b;
    return 1;
}

/* Simple named colour lookup for common X11 colour names */
static int parse_named_colour(const char *name, unsigned long *pixel) {
    struct { const char *name; unsigned long val; } colours[] = {
        {"black",    0x000000}, {"white",   0xFFFFFF},
        {"red",      0xFF0000}, {"green",   0x00FF00},
        {"blue",     0x0000FF}, {"yellow",  0xFFFF00},
        {"cyan",     0x00FFFF}, {"magenta", 0xFF00FF},
        {"gray",     0xBEBEBE}, {"grey",    0xBEBEBE},
        {"orange",   0xFFA500}, {"pink",    0xFFC0CB},
        {"brown",    0xA52A2A}, {"purple",  0x800080},
        {"darkblue", 0x00008B}, {"darkred", 0x8B0000},
        {"darkgreen",0x006400}, {"lightgray",0xD3D3D3},
        {"lightblue",0xADD8E6}, {"navy",    0x000080},
        {"maroon",   0x800000}, {"olive",   0x808000},
        {"teal",     0x008080}, {"silver",  0xC0C0C0},
        {"lime",     0x00FF00}, {"aqua",    0x00FFFF},
        {"fuchsia",  0xFF00FF}, {"coral",   0xFF7F50},
        {"gold",     0xFFD700}, {"khaki",   0xF0E68C},
        {"ivory",    0xFFFFF0}, {"beige",   0xF5F5DC},
        {"wheat",    0xF5DEB3}, {"tan",     0xD2B48C},
        {"chocolate",0xD2691E}, {"firebrick",0xB22222},
        {"crimson",  0xDC143C}, {"tomato",  0xFF6347},
        {"salmon",   0xFA8072}, {"plum",    0xDDA0DD},
        {"violet",   0xEE82EE}, {"orchid",  0xDA70D6},
        {"indigo",   0x4B0082}, {"sienna",  0xA0522D},
    };
    /* Case-insensitive name search */
    for (int i = 0; i < (int)(sizeof(colours)/sizeof(colours[0])); i++) {
        const char *cn = colours[i].name;
        const char *nm = name;
        int match = 1;
        while (*cn && *nm) {
            char a = *cn, b = *nm;
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = 0; break; }
            cn++; nm++;
        }
        if (match && *cn == '\0' && *nm == '\0') {
            *pixel = colours[i].val;
            return 1;
        }
    }
    return 0;
}

Status XParseColour(Display *display, Colourmap colourmap,
                   const char *spec, XColour *exact_def_return) {
    (void)display; (void)colourmap;
    if (!spec || !exact_def_return) return 0;

    unsigned long pix = 0;
    if (spec[0] == '#') {
        /* Parse hex colour: #RGB, #RRGGBB, #RRRRGGGGBBBB */
        int len = 0;
        for (const char *p = spec+1; *p; p++) len++;
        unsigned long val = 0;
        for (const char *p = spec+1; *p; p++) {
            char c = *p;
            int digit = 0;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            val = (val << 4) | digit;
        }
        if (len == 3) {
            unsigned r = (val >> 8) & 0xF;
            unsigned g = (val >> 4) & 0xF;
            unsigned b = val & 0xF;
            pix = (r*17 << 16) | (g*17 << 8) | (b*17);
        } else if (len == 6) {
            pix = val & 0xFFFFFF;
        } else if (len == 12) {
            /* 12-hex-digit colour: #RRRRGGGGBBBB -- extract top 8 bits of each */
            unsigned long rr = (val >> 24) & 0xFF00;
            unsigned long gg = (val >> 16) & 0xFF00;
            unsigned long bb = (val >>  8) & 0xFF00;
            pix = ((rr >> 8) << 16) | ((gg >> 8) << 8) | (bb >> 8);
        }
    } else {
        if (!parse_named_colour(spec, &pix))
            return 0;
    }

    exact_def_return->pixel = pix;
    exact_def_return->red   = (unsigned short)(((pix >> 16) & 0xFF) * 257);
    exact_def_return->green = (unsigned short)(((pix >>  8) & 0xFF) * 257);
    exact_def_return->blue  = (unsigned short)((pix & 0xFF) * 257);
    exact_def_return->flags = DoRed | DoGreen | DoBlue;
    return 1;
}

Status XAllocNamedColour(Display *display, Colourmap colourmap,
                        const char *colour_name,
                        XColour *screen_def_return,
                        XColour *exact_def_return) {
    XColour temp;
    if (!XParseColour(display, colourmap, colour_name, &temp))
        return 0;
    if (exact_def_return) *exact_def_return = temp;
    if (screen_def_return) {
        *screen_def_return = temp;
        XAllocColour(display, colourmap, screen_def_return);
    }
    return 1;
}

int XFreeColours(Display *display, Colourmap colourmap,
                unsigned long *pixels, int npixels,
                unsigned long planes) {
    (void)display; (void)colourmap; (void)pixels;
    (void)npixels; (void)planes;
    return 0;
}

Status XLookupColour(Display *display, Colourmap colourmap,
                    const char *colour_name,
                    XColour *exact_def_return,
                    XColour *screen_def_return) {
    return XAllocNamedColour(display, colourmap, colour_name,
                            screen_def_return, exact_def_return);
}

/* ---- Misc / stubs ------------------------------------------------ */

int XSetWindowBackground(Display *display, Window w,
                         unsigned long background_pixel) {
    (void)display; (void)w;
    if (g_x11) g_x11->bg_colour = background_pixel;
    return 0;
}

int XSetWindowBorder(Display *display, Window w,
                     unsigned long border_pixel) {
    (void)display; (void)w;
    if (g_x11) g_x11->border_colour = border_pixel;
    return 0;
}

int XBell(Display *display, int percent) {
    (void)display; (void)percent;
    return 0;
}

int XFree(void *data) {
    (void)data;
    /* free() is a no-op on EYN-OS bump allocator */
    return 0;
}

int XSetErrorHandler(void *handler) {
    (void)handler;
    return 0;
}

int XSetIOErrorHandler(void *handler) {
    (void)handler;
    return 0;
}

int XGrabPointer(Display *display, Window grab_window,
                 Bool owner_events, unsigned int event_mask,
                 int pointer_mode, int keyboard_mode,
                 Window confine_to, Cursor cursor, Time time) {
    (void)display; (void)grab_window; (void)owner_events;
    (void)event_mask; (void)pointer_mode; (void)keyboard_mode;
    (void)confine_to; (void)cursor; (void)time;
    return GrabSuccess;
}

int XUngrabPointer(Display *display, Time time) {
    (void)display; (void)time;
    return 0;
}

int XDefineCursor(Display *display, Window w, Cursor cursor) {
    (void)display; (void)w; (void)cursor;
    return 0;
}

int XUndefineCursor(Display *display, Window w) {
    (void)display; (void)w;
    return 0;
}

int XWarpPointer(Display *display, Window src_w, Window dest_w,
                 int src_x, int src_y,
                 unsigned int src_width, unsigned int src_height,
                 int dest_x, int dest_y) {
    (void)display; (void)src_w; (void)dest_w;
    (void)src_x; (void)src_y; (void)src_width; (void)src_height;
    (void)dest_x; (void)dest_y;
    return 0;
}

Font XLoadFont(Display *display, const char *name) {
    (void)display; (void)name;
    return 1; /* Dummy font ID */
}

int XSetFont(Display *display, GC gc, Font font) {
    (void)display; (void)font;
    if (gc) gc->values.font = font;
    return 0;
}

int XUnloadFont(Display *display, Font font) {
    (void)display; (void)font;
    return 0;
}

int XTextWidth(void *font_struct, const char *string, int count) {
    (void)font_struct; (void)string;
    /* 8 pixels per character with our 8×8 bitmap font */
    return count * 8;
}

/* ---- Xutil helpers ----------------------------------------------- */

void XSetWMNormalHints(Display *display, Window w, XSizeHints *hints) {
    (void)display; (void)w; (void)hints;
}

void XSetWMHints(Display *display, Window w, XWMHints *hints) {
    (void)display; (void)w; (void)hints;
}

void XSetClassHint(Display *display, Window w, XClassHint *class_hints) {
    (void)display; (void)w; (void)class_hints;
}

void XSetWMProperties(Display *display, Window w,
                      XTextProperty *window_name,
                      XTextProperty *icon_name,
                      char **argv, int argc,
                      XSizeHints *normal_hints,
                      XWMHints *wm_hints,
                      XClassHint *class_hints) {
    (void)icon_name; (void)argv; (void)argc;
    (void)normal_hints; (void)wm_hints; (void)class_hints;
    if (window_name && window_name->value)
        XStoreName(display, w, (const char *)window_name->value);
}

int XSetStandardProperties(Display *display, Window w,
                           const char *window_name,
                           const char *icon_name,
                           Pixmap icon_pixmap,
                           char **argv, int argc,
                           XSizeHints *hints) {
    (void)icon_name; (void)icon_pixmap; (void)argv; (void)argc; (void)hints;
    if (window_name) XStoreName(display, w, window_name);
    return 0;
}

XSizeHints *XAllocSizeHints(void) {
    return (XSizeHints *)calloc(1, sizeof(XSizeHints));
}

XWMHints *XAllocWMHints(void) {
    return (XWMHints *)calloc(1, sizeof(XWMHints));
}

XClassHint *XAllocClassHint(void) {
    return (XClassHint *)calloc(1, sizeof(XClassHint));
}

int XStringListToTextProperty(char **list, int count, XTextProperty *tp) {
    if (!list || count < 1 || !tp) return 0;
    tp->value    = (unsigned char *)list[0];
    tp->encoding = XA_STRING;
    tp->format   = 8;
    tp->nitems   = strlen(list[0]);
    return 1;
}

XVisualInfo *XGetVisualInfo(Display *display, long vinfo_mask,
                            XVisualInfo *vinfo_template,
                            int *nitems_return) {
    (void)display; (void)vinfo_mask; (void)vinfo_template;
    XVisualInfo *vi = (XVisualInfo *)calloc(1, sizeof(XVisualInfo));
    if (!vi) { if (nitems_return) *nitems_return = 0; return NULL; }
    vi->visual     = g_x11 ? &g_x11->visual : NULL;
    vi->visualid   = 1;
    vi->screen     = 0;
    vi->depth      = 24;
    vi->class_     = TrueColour;
    vi->red_mask   = 0xFF0000;
    vi->green_mask = 0x00FF00;
    vi->blue_mask  = 0x0000FF;
    vi->colourmap_size = 256;
    vi->bits_per_rgb  = 8;
    if (nitems_return) *nitems_return = 1;
    return vi;
}
