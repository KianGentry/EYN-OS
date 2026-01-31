#include <graphics/gfx.h>
#include <graphics/gfx_backend.h>

static const gfx_backend_t* g_backend;

void gfx_set_backend(const gfx_backend_t* backend) {
    g_backend = backend;
}

const gfx_backend_t* gfx_get_backend(void) {
    return g_backend;
}

void gfx_init_default(void) {
    if (g_backend) {
        return;
    }

#if defined(__i386__)
    extern const gfx_backend_t g_gfx_backend_vga;
    gfx_set_backend(&g_gfx_backend_vga);
#elif defined(__aarch64__)
    extern const gfx_backend_t g_gfx_backend_fb_simple;
    gfx_set_backend(&g_gfx_backend_fb_simple);
#else
    /* Unknown architecture: keep gfx disabled. */
    gfx_set_backend(0);
#endif
}

static const gfx_backend_t* gfx_b(void) {
    if (!g_backend) {
        gfx_init_default();
    }
    return g_backend;
}

int gfx_ready(void) {
    const gfx_backend_t* b = gfx_b();
    if (!b || !b->ready) return 0;
    return b->ready();
}

void gfx_putc(char c) {
    const gfx_backend_t* b = gfx_b();
    if (!b || !b->putc) return;
    b->putc(c);
}

void gfx_write(const char* s) {
    if (!s) return;
    const gfx_backend_t* b = gfx_b();
    if (!b) return;

    if (b->write) {
        b->write(s);
        return;
    }

    if (!b->putc) return;
    while (*s) {
        b->putc(*s++);
    }
}

void gfx_write_len(const char* s, uint32 len) {
    if (!s) return;
    const gfx_backend_t* b = gfx_b();
    if (!b || !b->putc) return;

    for (uint32 i = 0; i < len; i++) {
        b->putc(s[i]);
    }
}

void gfx_set_rgb(uint8 r, uint8 g, uint8 b) {
    const gfx_backend_t* be = gfx_b();
    if (be && be->set_rgb) {
        be->set_rgb(r, g, b);
    }
}

void gfx_set_bg_rgb(uint8 r, uint8 g, uint8 b) {
    const gfx_backend_t* be = gfx_b();
    if (be && be->set_bg_rgb) {
        be->set_bg_rgb(r, g, b);
    }
}

void gfx_clear(void) {
    const gfx_backend_t* b = gfx_b();
    if (b && b->clear) {
        b->clear();
    }
}

void gfx_clear_rgb(uint8 r, uint8 g, uint8 b) {
    const gfx_backend_t* be = gfx_b();
    if (be && be->clear_rgb) {
        be->clear_rgb(r, g, b);
        return;
    }

    /* Fallback: clear() if available. */
    gfx_clear();
}

void gfx_draw_pixel(int x, int y, uint8 r, uint8 g, uint8 b) {
    const gfx_backend_t* be = gfx_b();
    if (be && be->draw_pixel) {
        be->draw_pixel(x, y, r, g, b);
    }
}

void gfx_fill_rect(int x, int y, int w, int h, uint8 r, uint8 g, uint8 b) {
    const gfx_backend_t* be = gfx_b();
    if (be && be->fill_rect) {
        be->fill_rect(x, y, w, h, r, g, b);
    }
}

void gfx_flush(void) {
    const gfx_backend_t* b = gfx_b();
    if (b && b->flush) {
        b->flush();
    }
}

uint32 gfx_screen_width(void) {
    const gfx_backend_t* b = gfx_b();
    if (!b || !b->screen_width) return 0;
    return b->screen_width();
}

uint32 gfx_screen_height(void) {
    const gfx_backend_t* b = gfx_b();
    if (!b || !b->screen_height) return 0;
    return b->screen_height();
}
