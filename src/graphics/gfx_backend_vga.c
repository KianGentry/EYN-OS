#include <graphics/gfx_backend.h>

#include <drivers/vga.h>

static uint8 g_fg_r = 255;
static uint8 g_fg_g = 255;
static uint8 g_fg_b = 255;

static int vga_ready_impl(void) {
    /* If VGA is not initialized yet, screen dims will be 0. */
    return (vga_screen_width() != 0u && vga_screen_height() != 0u);
}

static void vga_putc_impl(char c) {
    drawText((int)(uint8)c, (int)g_fg_r, (int)g_fg_g, (int)g_fg_b);
}

static void vga_write_impl(const char* s) {
    if (!s) return;
    while (*s) {
        vga_putc_impl(*s++);
    }
}

static void vga_set_rgb_impl(uint8 r, uint8 g, uint8 b) {
    g_fg_r = r;
    g_fg_g = g;
    g_fg_b = b;
}

static void vga_clear_impl(void) {
    clearScreen();
}

static void vga_clear_rgb_impl(uint8 r, uint8 g, uint8 b) {
    /* drawRect() clips to the active framebuffer/backbuffer bounds. */
    drawRect(0, 0, 0x7fffffff, 0x7fffffff, (int)r, (int)g, (int)b);
}

static void vga_draw_pixel_impl(int x, int y, uint8 r, uint8 g, uint8 b) {
    drawPixel(x, y, (int)r, (int)g, (int)b);
}

static void vga_fill_rect_impl(int x, int y, int w, int h, uint8 r, uint8 g, uint8 b) {
    drawRect(x, y, w, h, (int)r, (int)g, (int)b);
}

static void vga_flush_impl(void) {
    vga_swap_buffers();
}

static uint32 vga_screen_w_impl(void) {
    return vga_screen_width();
}

static uint32 vga_screen_h_impl(void) {
    return vga_screen_height();
}

const gfx_backend_t g_gfx_backend_vga = {
    .name = "vga",
    .ready = vga_ready_impl,
    .putc = vga_putc_impl,
    .write = vga_write_impl,
    .set_rgb = vga_set_rgb_impl,
    .set_bg_rgb = 0,
    .clear = vga_clear_impl,
    .clear_rgb = vga_clear_rgb_impl,
    .draw_pixel = vga_draw_pixel_impl,
    .fill_rect = vga_fill_rect_impl,
    .flush = vga_flush_impl,
    .screen_width = vga_screen_w_impl,
    .screen_height = vga_screen_h_impl,
};
