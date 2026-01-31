#include <graphics/gfx_backend.h>

#include <drivers/aarch64/fb_simple.h>

static int fb_ready_impl(void) {
    return fb_simple_ready();
}

static void fb_putc_impl(char c) {
    fb_simple_putc(c);
}

static void fb_write_impl(const char* s) {
    fb_simple_write(s);
}

static void fb_set_rgb_impl(uint8 r, uint8 g, uint8 b) {
    fb_simple_set_rgb(r, g, b);
}

static void fb_set_bg_rgb_impl(uint8 r, uint8 g, uint8 b) {
    fb_simple_set_bg_rgb(r, g, b);
}

static void fb_clear_impl(void) {
    fb_simple_clear();
}

static void fb_clear_rgb_impl(uint8 r, uint8 g, uint8 b) {
    fb_simple_clear_rgb(r, g, b);
}

static void fb_draw_pixel_impl(int x, int y, uint8 r, uint8 g, uint8 b) {
    if (x < 0 || y < 0) return;
    fb_simple_draw_pixel((uint32)x, (uint32)y, r, g, b);
}

static void fb_fill_rect_impl(int x, int y, int w, int h, uint8 r, uint8 g, uint8 b) {
    if (w <= 0 || h <= 0) return;
    if (x < 0 || y < 0) return;
    fb_simple_fill_rect((uint32)x, (uint32)y, (uint32)w, (uint32)h, r, g, b);
}

static void fb_flush_impl(void) {
    fb_simple_flush();
}

static uint32 fb_screen_w_impl(void) {
    uint32 w = 0;
    if (fb_simple_get_info(0, &w, 0, 0, 0, 0) != 0) return 0;
    return w;
}

static uint32 fb_screen_h_impl(void) {
    uint32 h = 0;
    if (fb_simple_get_info(0, 0, &h, 0, 0, 0) != 0) return 0;
    return h;
}

const gfx_backend_t g_gfx_backend_fb_simple = {
    .name = "fb_simple",
    .ready = fb_ready_impl,
    .putc = fb_putc_impl,
    .write = fb_write_impl,
    .set_rgb = fb_set_rgb_impl,
    .set_bg_rgb = fb_set_bg_rgb_impl,
    .clear = fb_clear_impl,
    .clear_rgb = fb_clear_rgb_impl,
    .draw_pixel = fb_draw_pixel_impl,
    .fill_rect = fb_fill_rect_impl,
    .flush = fb_flush_impl,
    .screen_width = fb_screen_w_impl,
    .screen_height = fb_screen_h_impl,
};
