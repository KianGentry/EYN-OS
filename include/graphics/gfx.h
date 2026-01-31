#ifndef EYNOS_GFX_H
#define EYNOS_GFX_H

#include <misc/types.h>

/*
 * Arch-neutral graphics facade.
 *
 * Design goals:
 * - Shared code calls into this API with no architecture-specific includes.
 * - Backend specifics (VGA vs framebuffer) live behind a small vtable.
 * - Keep it small and low-risk: this is a thin wrapper over existing drivers.
 */

typedef struct gfx_backend gfx_backend_t;

/* Backend registration (typically done by gfx_init_default()). */
void gfx_set_backend(const gfx_backend_t* backend);
const gfx_backend_t* gfx_get_backend(void);

/*
 * Initialize the default backend for the current build target.
 * Safe to call multiple times.
 */
void gfx_init_default(void);

/* Capability query */
int gfx_ready(void);

/* Console-ish output (graphical side only). */
void gfx_putc(char c);
void gfx_write(const char* s);
void gfx_write_len(const char* s, uint32 len);

/* Color state used by backends that support it. */
void gfx_set_rgb(uint8 r, uint8 g, uint8 b);
void gfx_set_bg_rgb(uint8 r, uint8 g, uint8 b);

/* Basic drawing */
void gfx_clear(void);
void gfx_clear_rgb(uint8 r, uint8 g, uint8 b);
void gfx_draw_pixel(int x, int y, uint8 r, uint8 g, uint8 b);
void gfx_fill_rect(int x, int y, int w, int h, uint8 r, uint8 g, uint8 b);

/* Optional: present/flush if the backend buffers. */
void gfx_flush(void);

/* Optional screen geometry (0 if unknown/not ready). */
uint32 gfx_screen_width(void);
uint32 gfx_screen_height(void);

#endif
