#include <vga.h>

#include <drivers/aarch64/fb_simple.h>
#include <misc/types.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// AArch64 VGA compatibility shim:
// The existing TUI/tiler stack is wired to the VGA drawing API. On AArch64-full
// we render those calls into the detected simple-framebuffer (ramfb).

int width = 0;
int height = 0;

int vga_default_r = 200;
int vga_default_g = 200;
int vga_default_b = 200;

static int g_active_window = 0;
static int g_vsync_enabled = 1;
static int g_dirty_strategy = 0;

// Optional: while redirect is active, stream output to a vterm.
static int g_shell_redirect_stream_vterm = -1;

int shell_redirect_color_r = 200;
int shell_redirect_color_g = 200;
int shell_redirect_color_b = 200;

int shell_redirect_active = 0;
int shell_redirect_pos = 0;
char shell_redirect_buf[SHELL_REDIRECT_BUF_SIZE];

unsigned char shell_redirect_r[SHELL_REDIRECT_BUF_SIZE];
unsigned char shell_redirect_g[SHELL_REDIRECT_BUF_SIZE];
unsigned char shell_redirect_b[SHELL_REDIRECT_BUF_SIZE];

shell_redirect_icon_t shell_redirect_icons[SHELL_REDIRECT_ICON_MAX];
int shell_redirect_icon_count = 0;

static const char* g_fb_fmt_str = 0;

static inline int fb_query(uint64* out_base, uint32* out_w, uint32* out_h, uint32* out_stride, uint32* out_bpp) {
    const char* fmt = 0;
    if (fb_simple_get_info(out_base, out_w, out_h, out_stride, out_bpp, &fmt) != 0) return -1;
    if (!out_w || !out_h) return -1;
    if (width == 0) width = (int)(*out_w);
    if (height == 0) height = (int)(*out_h);
    if (!g_fb_fmt_str) g_fb_fmt_str = fmt;
    return 0;
}

static inline uint32 pack_rgb(uint8 r, uint8 g, uint8 b) {
    // Match fb_simple's supported formats.
    if (g_fb_fmt_str && (strstr(g_fb_fmt_str, "XBGR") || strstr(g_fb_fmt_str, "xbgr"))) {
        return ((uint32)b << 16) | ((uint32)g << 8) | (uint32)r;
    }
    return ((uint32)r << 16) | ((uint32)g << 8) | (uint32)b;
}

static inline void fb_flush_full(void) {
    if (!fb_simple_ready()) return;
    fb_simple_flush();
}

void vga_set_color(int nr, int ng, int nb) {
    vga_default_r = nr;
    vga_default_g = ng;
    vga_default_b = nb;
}

int vga_text_cell_w(void) { return 8; }

int vga_text_cell_h(void) {
    // The i386 UI expects 8x16 proportions when the default font is unscii-16.
    // For bring-up we render the 8x8 fallback glyphs doubled vertically.
    return 16;
}

int vga_font_glyph_h(int font_handle) {
    (void)font_handle;
    return vga_text_cell_h();
}

int vga_font_acquire_hex(uint8 drive, const char* path) { (void)drive; (void)path; return -1; }
void vga_font_release(int font_handle) { (void)font_handle; }
int vga_system_font_acquire(void) { return 0; }
int vga_system_font_set(uint8 drive, const char* path) { (void)drive; (void)path; return 0; }

void drawPixel(int x, int y, int r, int g, int b) {
    if (!fb_simple_ready()) return;
    if (x < 0 || y < 0) return;

    uint64 base = 0;
    uint32 fb_w = 0, fb_h = 0, stride = 0, bpp = 0;
    if (fb_query(&base, &fb_w, &fb_h, &stride, &bpp) != 0) return;
    if (bpp != 32) return;
    if ((uint32)x >= fb_w || (uint32)y >= fb_h) return;

    volatile uint32* p = (volatile uint32*)(uintptr_t)(base + (uint64)(uint32)y * (uint64)stride + (uint64)(uint32)x * 4ull);
    *p = pack_rgb((uint8)r, (uint8)g, (uint8)b);
}

void vga_drawPixel_fb(int x, int y, int r, int g, int b) { drawPixel(x, y, r, g, b); }
void vga_drawPixel_bb(int x, int y, int r, int g, int b) { drawPixel(x, y, r, g, b); }

void vga_blendPixel_bb(int x, int y, int r, int g, int b, int a) {
    // Best-effort: no readback blending; treat alpha as binary.
    if (a >= 128) drawPixel(x, y, r, g, b);
}

void drawRect(int x, int y, int w, int h, int r, int g, int b) {
    if (!fb_simple_ready()) return;
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    fb_simple_fill_rect((uint32)x, (uint32)y, (uint32)w, (uint32)h, (uint8)r, (uint8)g, (uint8)b);
}

void vga_fillRect_fb(int x, int y, int w, int h, int r, int g, int b) { drawRect(x, y, w, h, r, g, b); }

void drawLine(int x1, int y1, int x2, int y2, int r, int g, int b) {
    // Simple Bresenham
    int dx = (x2 > x1) ? (x2 - x1) : (x1 - x2);
    int sx = (x1 < x2) ? 1 : -1;
    int dy = (y2 > y1) ? (y1 - y2) : (y2 - y1);
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx + dy;
    while (1) {
        drawPixel(x1, y1, r, g, b);
        if (x1 == x2 && y1 == y2) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

void clearScreen(void) {
    if (!fb_simple_ready()) return;
    fb_simple_clear_rgb(0, 0, 0);
    fb_flush_full();
}

void drawCharAt_font(int font_handle, int x, int y, int charnum, int r, int g, int b) {
    (void)font_handle;
    if (!fb_simple_ready()) return;
    if (x < 0 || y < 0) return;
    fb_simple_draw_glyph8x16_doubled((uint32)x, (uint32)y, (uint8)charnum, (uint8)r, (uint8)g, (uint8)b);
}

void drawCharAt(int x, int y, int charnum, int r, int g, int b) {
    drawCharAt_font(0, x, y, charnum, r, g, b);
}

void drawTextAt(int x, int y, const char* text, int r, int g, int b) {
    if (!text) return;
    int cx = x;
    int cw = vga_text_cell_w();
    for (int i = 0; text[i]; ++i) {
        if (text[i] == '\n') {
            y += vga_text_cell_h();
            cx = x;
            continue;
        }
        drawCharAt(cx, y, (unsigned char)text[i], r, g, b);
        cx += cw;
    }
}

void drawText(int charnum, int r, int g, int b) {
    // Minimal cursorless implementation: draw at (0,0) + advance not tracked.
    drawCharAt(0, 0, charnum, r, g, b);
}

void drawText_bold(int charnum, int r, int g, int b) { drawText(charnum, r, g, b); }
void drawText_italic(int charnum, int r, int g, int b) { drawText(charnum, r, g, b); }
void drawText_large(int charnum, int r, int g, int b) { drawText(charnum, r, g, b); }

// Shell redirect icon markers
void shell_register_redirect_icon(const char* ext) {
    if (!ext || !ext[0]) return;
    if (shell_redirect_icon_count >= SHELL_REDIRECT_ICON_MAX) return;
    shell_redirect_icon_t* it = &shell_redirect_icons[shell_redirect_icon_count++];
    it->pos = shell_redirect_pos;
    strncpy(it->ext, ext, sizeof(it->ext) - 1);
    it->ext[sizeof(it->ext) - 1] = '\0';
}

void start_shell_redirect(void) {
    shell_redirect_active = 1;
    shell_redirect_pos = 0;
    shell_redirect_buf[0] = '\0';
    shell_redirect_icon_count = 0;
}

void stop_shell_redirect(void) {
    if (shell_redirect_pos < 0) shell_redirect_pos = 0;
    if (shell_redirect_pos >= SHELL_REDIRECT_BUF_SIZE) shell_redirect_pos = SHELL_REDIRECT_BUF_SIZE - 1;
    shell_redirect_buf[shell_redirect_pos] = '\0';
    shell_redirect_active = 0;
}

void vga_set_shell_redirect_stream_vterm(int vterm_idx) { g_shell_redirect_stream_vterm = vterm_idx; }
void vga_clear_shell_redirect_stream_vterm(void) { g_shell_redirect_stream_vterm = -1; }

// Double buffering / dirty-rect API: we draw directly to FB and flush on swap.
void vga_init_double_buffer(void) { }
void vga_mark_dirty_rect(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void vga_begin_frame(void) { }
void vga_set_swap_exclude(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void vga_clear_swap_exclude(void) { }

void vga_blit_backbuffer_region_to_fb(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
    fb_flush_full();
}

void vga_swap_buffers(void) { fb_flush_full(); }

void vga_blit_rgb565_bb(int dst_x, int dst_y, const uint16_t* src, int src_w, int src_h) {
    if (!src || src_w <= 0 || src_h <= 0) return;
    for (int y = 0; y < src_h; ++y) {
        for (int x = 0; x < src_w; ++x) {
            uint16_t px = src[y * src_w + x];
            int r = (px >> 11) & 0x1F;
            int g = (px >> 5) & 0x3F;
            int b = (px) & 0x1F;
            r = (r * 255) / 31;
            g = (g * 255) / 63;
            b = (b * 255) / 31;
            vga_drawPixel_bb(dst_x + x, dst_y + y, r, g, b);
        }
    }
}

void vga_blit_rgb565_scaled_bb(int dst_x, int dst_y, int dst_w, int dst_h,
	const uint16_t* src, int src_w, int src_h) {
    if (!src || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;
    for (int y = 0; y < dst_h; ++y) {
        int sy = (y * src_h) / dst_h;
        for (int x = 0; x < dst_w; ++x) {
            int sx = (x * src_w) / dst_w;
            uint16_t px = src[sy * src_w + sx];
            int r = (px >> 11) & 0x1F;
            int g = (px >> 5) & 0x3F;
            int b = (px) & 0x1F;
            r = (r * 255) / 31;
            g = (g * 255) / 63;
            b = (b * 255) / 31;
            vga_drawPixel_bb(dst_x + x, dst_y + y, r, g, b);
        }
    }
}

void vga_darkenRect_bb(int x, int y, int w, int h, int factor) {
    // No readback path implemented; approximate by drawing a semi-opaque black overlay.
    (void)factor;
    // Very conservative: do nothing.
    (void)x; (void)y; (void)w; (void)h;
}

int vga_get_fb_bpp_bytes(void) { return 4; }

int vga_capture_fb_region(int x, int y, int w, int h, unsigned char* out_buf, int out_buf_len) {
    uint64 base = 0;
    uint32 fb_w = 0, fb_h = 0, stride = 0, bpp = 0;
    if (fb_query(&base, &fb_w, &fb_h, &stride, &bpp) != 0) return -1;
    if (bpp != 32) return -1;
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return -1;
    if ((uint32)x >= fb_w || (uint32)y >= fb_h) return -1;
    if ((uint32)(x + w) > fb_w) w = (int)(fb_w - (uint32)x);
    if ((uint32)(y + h) > fb_h) h = (int)(fb_h - (uint32)y);
    int needed = w * h * 4;
    if (!out_buf || out_buf_len < needed) return -1;

    const uint8* fb = (const uint8*)(uintptr_t)base;
    for (int row = 0; row < h; ++row) {
        const uint8* src = fb + (uint32)(y + row) * stride + (uint32)x * 4u;
        memcpy(out_buf + row * w * 4, src, (size_t)(w * 4));
    }
    return needed;
}

int vga_restore_fb_region(int x, int y, int w, int h, const unsigned char* in_buf, int in_buf_len) {
    uint64 base = 0;
    uint32 fb_w = 0, fb_h = 0, stride = 0, bpp = 0;
    if (fb_query(&base, &fb_w, &fb_h, &stride, &bpp) != 0) return -1;
    if (bpp != 32) return -1;
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return -1;
    if ((uint32)x >= fb_w || (uint32)y >= fb_h) return -1;
    if ((uint32)(x + w) > fb_w) w = (int)(fb_w - (uint32)x);
    if ((uint32)(y + h) > fb_h) h = (int)(fb_h - (uint32)y);
    int needed = w * h * 4;
    if (!in_buf || in_buf_len < needed) return -1;

    uint8* fb = (uint8*)(uintptr_t)base;
    for (int row = 0; row < h; ++row) {
        uint8* dst = fb + (uint32)(y + row) * stride + (uint32)x * 4u;
        memcpy(dst, in_buf + row * w * 4, (size_t)(w * 4));
    }
    fb_flush_full();
    return needed;
}

void vga_wait_vblank(void) { }
void vga_set_vsync_enabled(int enabled) { g_vsync_enabled = enabled ? 1 : 0; }
int vga_get_vsync_enabled(void) { return g_vsync_enabled; }
void vga_set_dirty_strategy(int strategy) { g_dirty_strategy = strategy; }
int vga_get_dirty_strategy(void) { return g_dirty_strategy; }

uint32 vga_screen_width(void) {
    uint64 base = 0; uint32 w = 0, h = 0, stride = 0, bpp = 0;
    if (fb_query(&base, &w, &h, &stride, &bpp) != 0) return 0;
    return w;
}

uint32 vga_screen_height(void) {
    uint64 base = 0; uint32 w = 0, h = 0, stride = 0, bpp = 0;
    if (fb_query(&base, &w, &h, &stride, &bpp) != 0) return 0;
    return h;
}

// Window API: keep minimal state so callers compile.
void vga_windows_init_2x2(void) { g_active_window = 0; }
void vga_set_active_window(int index) { g_active_window = index; }
int vga_get_active_window(void) { return g_active_window; }
void vga_clear_window(int index) { (void)index; }
void vga_window_set_title(int index, const char* title) { (void)index; (void)title; }

void render_markdown(const char* content) { (void)content; }
