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
void drawText_bold(int charnum, int r, int g, int b);
void drawText_italic(int charnum, int r, int g, int b);
void drawText_large(int charnum, int r, int g, int b);
void drawLine(int x1, int y1, int x2, int y2, int r, int g, int b);
void drawPixel(int x, int y, int r, int g, int b);
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

// Shell logging variables
extern char* shell_log_buf;
extern int shell_log_buf_size;
extern int shell_log_pos;
#define LOG_BUF_SIZE 65536

// Double buffer integration
void vga_swap_buffers(void);

// Global variables
extern int width, height;
extern int r, g, b;

// Printf function
void printf(const char* format, ...);
int snprintf(char *str, size_t size, const char *format, ...);

#endif // VGA_H