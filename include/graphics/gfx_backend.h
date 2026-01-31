#ifndef EYNOS_GFX_BACKEND_H
#define EYNOS_GFX_BACKEND_H

#include <misc/types.h>

/*
 * Internal backend interface for the arch-neutral gfx facade.
 *
 * Backends are expected to be tiny shims over existing drivers.
 * All callbacks must be freestanding-safe and avoid heap allocations.
 */

typedef struct gfx_backend {
    const char* name;

    int (*ready)(void);

    void (*putc)(char c);
    void (*write)(const char* s);

    void (*set_rgb)(uint8 r, uint8 g, uint8 b);
    void (*set_bg_rgb)(uint8 r, uint8 g, uint8 b);

    void (*clear)(void);
    void (*clear_rgb)(uint8 r, uint8 g, uint8 b);

    void (*draw_pixel)(int x, int y, uint8 r, uint8 g, uint8 b);
    void (*fill_rect)(int x, int y, int w, int h, uint8 r, uint8 g, uint8 b);

    void (*flush)(void);

    uint32 (*screen_width)(void);
    uint32 (*screen_height)(void);
} gfx_backend_t;

#endif
