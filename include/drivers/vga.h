#ifndef VGA_H
#define VGA_H

#include <types.h>
#include <multiboot.h>
#include <stddef.h>

// Shell redirection buffer size
#define SHELL_REDIRECT_BUF_SIZE 4096

// Misc
void render_markdown(const char* content);
void vga_set_color(int nr, int ng, int nb);

// Function prototypes
void init_dynamic_log_buffer(void);
void shell_log_enable(void);
void shell_log_disable(void);
void shell_log_flush(void);

// Drawing functions
void drawRect(int x, int y, int w, int h, int r, int g, int b);
void drawText(int charnum, int r, int g, int b);
// Draw a NUL-terminated string at pixel coordinates (x,y) using 8x8 font
void drawTextAt(int x, int y, const char* text, int r, int g, int b);
// Draw a single character at pixel coords (x,y)
void drawCharAt(int x, int y, int charnum, int r, int g, int b);
void drawText_bold(int charnum, int r, int g, int b);
void drawText_italic(int charnum, int r, int g, int b);
void drawText_large(int charnum, int r, int g, int b);
void drawLine(int x1, int y1, int x2, int y2, int r, int g, int b);
void drawPixel(int x, int y, int r, int g, int b);
// Draw to backbuffer only (no dirty mark). Call vga_mark_dirty_rect separately.
void vga_drawPixel_bb(int x, int y, int r, int g, int b);
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
extern char shell_redirect_buf[SHELL_REDIRECT_BUF_SIZE];
// Color used for the last redirected output (set by printf while redirect active)
extern int shell_redirect_color_r;
extern int shell_redirect_color_g;
extern int shell_redirect_color_b;
// Per-character color for redirected output (parallel to shell_redirect_buf)
extern unsigned char shell_redirect_r[SHELL_REDIRECT_BUF_SIZE];
extern unsigned char shell_redirect_g[SHELL_REDIRECT_BUF_SIZE];
extern unsigned char shell_redirect_b[SHELL_REDIRECT_BUF_SIZE];

// Shell logging variables
extern char* shell_log_buf;
extern int shell_log_buf_size;
extern int shell_log_pos;
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
// Framebuffer region helpers for save-under cursors
int vga_get_fb_bpp_bytes(void);
int vga_capture_fb_region(int x, int y, int w, int h, unsigned char* out_buf, int out_buf_len);
int vga_restore_fb_region(int x, int y, int w, int h, const unsigned char* in_buf, int in_buf_len);
// Best-effort VBlank sync (safe no-op if unsupported). Waits for start of vblank.
void vga_wait_vblank(void);

// Global variables
extern int width, height;
extern int r, g, b;

// Printf function
void printf(const char* format, ...);
int snprintf(char *str, size_t size, const char *format, ...);

#endif // VGA_H