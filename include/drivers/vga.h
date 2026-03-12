#ifndef VGA_H
#define VGA_H

#include <misc/types.h>
#include <multiboot.h>
#include <stdint.h>
#include <stddef.h>

// Shell redirection buffer size (lowered for small-memory systems)
// Reducing this value reduces static RAM used by the redirect buffers
// and is safe because code already bounds copy operations.
#define SHELL_REDIRECT_BUF_SIZE 1024

// Misc
void render_markdown(const char* content);
void vga_set_colour(int nr, int ng, int nb);

// Function prototypes
void init_dynamic_log_buffer(void);
void shell_log_enable(void);
void shell_log_disable(void);
int shell_log_is_enabled(void);
void shell_log_flush(void);

// Drawing functions
void drawRect(int x, int y, int w, int h, int r, int g, int b);
void drawText(int charnum, int r, int g, int b);
// Draw a NUL-terminated string at pixel coordinates (x,y) using 8x8 font
void drawTextAt(int x, int y, const char* text, int r, int g, int b);
// Draw a single character at pixel coords (x,y)
void drawCharAt(int x, int y, int charnum, int r, int g, int b);
// Draw a single character at pixel coords (x,y) using a specific font handle.
// font_handle 0 is the built-in fallback font.
void drawCharAt_font(int font_handle, int x, int y, int charnum, int r, int g, int b);

// Acquire a bitmap font from a .hex file via the VFS. Returns a font handle (>0) on success.
// The returned handle is refcounted; call vga_font_release() when no longer needed.
// On failure returns <0.
int vga_font_acquire_hex(uint8 drive, const char* path);
// Release a previously-acquired font handle (safe no-op for handle <= 0).
void vga_font_release(int font_handle);

// Return the glyph height (in pixels) of a font handle.
// Returns 8 for built-in/8x8 fonts, 16 for 8x16 fonts.
// Safe for handle <= 0 (returns 8).
int vga_font_glyph_height(int font_handle);

// Acquire the system default font handle (refcounted). Returns >0 when the
// configured system font is available; returns 0 to indicate the built-in
// fallback should be used.
// Call vga_font_release() when no longer needed.
int vga_system_font_acquire(void);

// Set the system default font used by drawText/drawCharAt when font_handle==0.
// On success, loads the font from disk into RAM and releases the previous system font.
// Use path="builtin" (or NULL/empty) to revert to built-in fallback.
// Returns 0 on success, -1 on failure.
int vga_system_font_set(uint8 drive, const char* path);

// Text cell metrics for the currently active system font used by drawText/drawCharAt.
// Width is always 8; height is 8 or 16 depending on the loaded system font.
int vga_text_cell_w(void);
int vga_text_cell_h(void);
// Query glyph height (8 or 16) for a specific font handle (0 = built-in).
int vga_font_glyph_h(int font_handle);
void drawText_bold(int charnum, int r, int g, int b);
void drawText_italic(int charnum, int r, int g, int b);
void drawText_large(int charnum, int r, int g, int b);
void drawLine(int x1, int y1, int x2, int y2, int r, int g, int b);
void drawPixel(int x, int y, int r, int g, int b);
// Draw to backbuffer only (no dirty mark). Call vga_mark_dirty_rect separately.
void vga_drawPixel_bb(int x, int y, int r, int g, int b);
// Blend an RGBA pixel into the backbuffer (or framebuffer fallback).
// alpha 0..255 where 0 is transparent and 255 is opaque.
void vga_blendPixel_bb(int x, int y, int r, int g, int b, int a);
void clearScreen(void);

// windowing (viewport) api - up to 4 windows in a 2x2 grid
void vga_windows_init_2x2(void);
void vga_set_active_window(int index);
int vga_get_active_window(void);
void vga_clear_window(int index);
void vga_window_set_title(int index, const char* title);

// Shell redirection functions
void start_shell_redirect(void);
void stop_shell_redirect(void);
// Optional: while shell redirection is active, also stream output to a vterm.
// This is used to keep the GUI responsive while a ring3 task is running and
// commands are executed from the IRQ-driven input pump.
void vga_set_shell_redirect_stream_vterm(int vterm_idx);
void vga_clear_shell_redirect_stream_vterm(void);
// True while shell output is being redirected into shell_redirect_buf.
extern int shell_redirect_active;
// When set, printf also captures output into the redirect buffer even if redirect is not active.
extern int g_shell_capture_mode;
extern char shell_redirect_buf[SHELL_REDIRECT_BUF_SIZE];
// Current write position in shell_redirect_buf while redirect is active.
extern int shell_redirect_pos;
// Colour used for the last redirected output (set by printf while redirect active)
extern int shell_redirect_colour_r;
extern int shell_redirect_colour_g;
extern int shell_redirect_colour_b;
// Per-character colour for redirected output (parallel to shell_redirect_buf)
extern unsigned char shell_redirect_r[SHELL_REDIRECT_BUF_SIZE];
extern unsigned char shell_redirect_g[SHELL_REDIRECT_BUF_SIZE];
extern unsigned char shell_redirect_b[SHELL_REDIRECT_BUF_SIZE];

// Icon markers recorded during shell redirect: used by shell utilities (ls) to mark
// where an icon should be drawn for a filename printed into the redirect buffer.
#define SHELL_REDIRECT_ICON_MAX 128
typedef struct { int pos; char ext[16]; } shell_redirect_icon_t;
extern shell_redirect_icon_t shell_redirect_icons[SHELL_REDIRECT_ICON_MAX];
extern int shell_redirect_icon_count;
// Register an icon for the next bytes to be emitted into shell_redirect_buf.
void shell_register_redirect_icon(const char* ext);

// Shell logging variables
extern int shell_log_active;
extern char* shell_log_buf;
extern int shell_log_buf_size;
extern int shell_log_pos;
extern int shell_log_line_count;
extern int shell_log_line_starts[1001];
extern int shell_log_current_line_start;
#define LOG_BUF_SIZE 65536

// Double buffer integration
void vga_swap_buffers(void);
// Initialize the software backbuffer (safe no-op if allocation fails)
void vga_init_double_buffer(void);
// Mark a rectangle in the backbuffer as dirty (so swap will blit it)
void vga_mark_dirty_rect(int x, int y, int w, int h);
// Begin a new frame (reset dirty rect tracking)
void vga_begin_frame(void);
// Optionally exclude a rectangle from swap (e.g., preserve software cursor)
void vga_set_swap_exclude(int x, int y, int w, int h);
void vga_clear_swap_exclude(void);
// Overlay helpers (draw directly to framebuffer without touching backbuffer)
void vga_drawPixel_fb(int x, int y, int r, int g, int b);
void vga_blit_backbuffer_region_to_fb(int x, int y, int w, int h);
// Returns 1 if a software backbuffer is allocated and being used, else 0.
int vga_has_backbuffer(void);
// Blit a RGB565LE source image into the backbuffer, scaling (nearest-neighbor)
// into the destination rectangle. Call vga_mark_dirty_rect() separately.
void vga_blit_rgb565_scaled_bb(int dst_x, int dst_y, int dst_w, int dst_h,
	const uint16_t* src, int src_w, int src_h);
// Blit a RGB565LE source image into the backbuffer without scaling (clipped).
// Call vga_mark_dirty_rect() separately.
void vga_blit_rgb565_bb(int dst_x, int dst_y, const uint16_t* src, int src_w, int src_h);
// Fill a rectangle directly into the physical framebuffer (overlay), clipped to screen
void vga_fillRect_fb(int x, int y, int w, int h, int r, int g, int b);
// Multiply-darken a rectangle in the backbuffer (or framebuffer if no backbuffer):
// out.rgb = (in.rgb * factor + 127) / 255. factor=128 ~= 50% black overlay.
void vga_darkenRect_bb(int x, int y, int w, int h, int factor);
// Framebuffer region helpers for save-under cursors
int vga_get_fb_bpp_bytes(void);
int vga_capture_fb_region(int x, int y, int w, int h, unsigned char* out_buf, int out_buf_len);
int vga_restore_fb_region(int x, int y, int w, int h, const unsigned char* in_buf, int in_buf_len);
// Best-effort VBlank sync (safe no-op if unsupported). Waits for start of vblank.
void vga_wait_vblank(void);
// Enable/disable vblank sync during swaps (1=enabled default; 0=disabled for max throughput)
void vga_set_vsync_enabled(int enabled);
int vga_get_vsync_enabled(void);
// Dirty rect blit strategy: 0 = smart/multi-rect, 1 = single bounding rect per swap
void vga_set_dirty_strategy(int strategy);
int vga_get_dirty_strategy(void);

// Global variables
extern int width, height;
extern int vga_default_r, vga_default_g, vga_default_b;

// Printf function
void printf(const char* format, ...);
int snprintf(char *str, size_t size, const char *format, ...);

#endif // VGA_H