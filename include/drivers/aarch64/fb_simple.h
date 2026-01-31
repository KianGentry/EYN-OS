#ifndef EYNOS_AARCH64_FB_SIMPLE_H
#define EYNOS_AARCH64_FB_SIMPLE_H

#include <misc/types.h>

/*
 * Minimal simple-framebuffer text console for early AArch64 bring-up.
 */
int fb_simple_init(uint64 dtb_ptr);
int fb_simple_ready(void);
void fb_simple_putc(char c);
void fb_simple_write(const char* s);
void fb_simple_clear(void);

/* Optional enhancements used by the arch-neutral gfx facade. */
void fb_simple_set_rgb(uint8 r, uint8 g, uint8 b);
void fb_simple_set_bg_rgb(uint8 r, uint8 g, uint8 b);
void fb_simple_clear_rgb(uint8 r, uint8 g, uint8 b);
void fb_simple_draw_pixel(uint32 x, uint32 y, uint8 r, uint8 g, uint8 b);
void fb_simple_fill_rect(uint32 x, uint32 y, uint32 w, uint32 h, uint8 r, uint8 g, uint8 b);
void fb_simple_flush(void);

/*
 * Query the detected framebuffer parameters.
 * Returns 0 on success, -1 if the framebuffer is not initialized.
 */
int fb_simple_get_info(uint64* out_base,
					   uint32* out_width,
					   uint32* out_height,
					   uint32* out_stride,
					   uint32* out_bpp,
					   const char** out_format);

/*
 * fb_simple_init() diagnostics: non-zero means initialization failed.
 * Values are stable for debugging but not a user-facing API.
 */
int fb_simple_last_error(void);

#endif
