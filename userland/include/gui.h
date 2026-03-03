// Minimal GUI/Tiling syscalls.
#include <stdint.h>
// Handles are small ints. Handle 0 refers to the calling task's existing tile.

int gui_create(const char* title, const char* status_left);
int gui_set_title(int handle, const char* title);

// Attach a GUI client to the calling task's current tile (handle 0).
// This avoids creating a new tile, which is important while ring3 is active
// because mouse-based focus switching isn't pumped.
int gui_attach(const char* title, const char* status_left);

// Immediate-mode user GUI

typedef struct {
	unsigned char r, g, b, _pad;
} gui_rgb_t;

typedef struct {
	int x, y, w, h;
	unsigned char r, g, b, _pad;
} gui_rect_t;

typedef struct {
	int x, y;
	unsigned char r, g, b, _pad;
	const char* text;
} gui_text_t;

typedef struct {
	unsigned int type;
	int a;
	int b;
	int c;
	int d;
} gui_event_t;

enum {
	GUI_EVENT_NONE = 0,
	GUI_EVENT_KEY = 1,
	GUI_EVENT_MOUSE = 2,
	GUI_EVENT_CLOSE = 3,  /* Window close request (user clicked X / Super+Q) */
};

typedef struct {
	int x1, y1, x2, y2;
	unsigned char r, g, b, _pad;
} gui_line_t;

typedef struct {
	int w, h;
} gui_size_t;

int gui_begin(int handle);
int gui_clear(int handle, const gui_rgb_t* rgb);
int gui_fill_rect(int handle, const gui_rect_t* rect);
int gui_draw_text(int handle, const gui_text_t* cmd);
int gui_draw_line(int handle, const gui_line_t* cmd);
int gui_present(int handle);

int gui_get_content_size(int handle, gui_size_t* out_size);

// Set the bitmap font for a GUI handle from a .hex file path.
// Passing NULL or an empty string resets to the built-in kernel font.
int gui_set_font(int handle, const char* hex_path);

// Enable or disable continuous redraw for animated clients.
int gui_set_continuous_redraw(int handle, int enabled);

// Blit a RGB565LE framebuffer into the GUI content area.
typedef struct {
	int src_w, src_h;
	const uint16_t* pixels; // RGB565LE
	int dst_w, dst_h;       // <=0 means use content size
} gui_blit_rgb565_t;
int gui_blit_rgb565(int handle, const gui_blit_rgb565_t* cmd);

// Draw a named icon from the kernel-side icon cache at (x, y).
// icon_name: base name such as "file_c", "dir_empty", "file_none".
typedef struct {
	int x, y;
	const char* icon_name;
} gui_icon_t;
int gui_draw_icon(int handle, const gui_icon_t* cmd);

// Draw a 1-pixel outlined rectangle (border only, no fill).
// Uses the same gui_rect_t layout as gui_fill_rect.
int gui_outline_rect(int handle, const gui_rect_t* rect);

// Draw a single character at pixel position (x, y). More efficient than
// gui_draw_text for per-character rendering (e.g. text editors).
typedef struct {
	int x, y, ch;
	unsigned char r, g, b, _pad;
} gui_char_t;
int gui_draw_char(int handle, const gui_char_t* cmd);

// Query font metrics for the GUI's current font.
// Returns cell width and height in pixels (typically 8xN).
typedef struct {
	int char_w, char_h;
} gui_font_metrics_t;
int gui_get_font_metrics(int handle, gui_font_metrics_t* out);

/*
 * Key code constants (from tui_read_key scancode table).
 * These match the values delivered via GUI_EVENT_KEY.
 */
#define GUI_KEY_UP        0x1001
#define GUI_KEY_DOWN      0x1002
#define GUI_KEY_LEFT      0x1003
#define GUI_KEY_RIGHT     0x1004
#define GUI_KEY_DELETE    0x1005
#define GUI_KEY_HOME      0x1006
#define GUI_KEY_END       0x1007
#define GUI_KEY_PGUP      0x1008
#define GUI_KEY_PGDN      0x1009
#define GUI_KEY_SHIFT     0x3000  /* OR'd with arrow keys when Shift is held */
#define GUI_KEY_SUPER     0x4000  /* OR'd when Super is held */
#define GUI_KEY_CTRL      0x8000  /* OR'd with nav keys when Ctrl is held */

/* Ctrl combos */
#define GUI_KEY_CTRL_S    0x2001  /* Ctrl+S / Ctrl+O (save) */
#define GUI_KEY_CTRL_Q    0x2101  /* Ctrl+Q (quit) */
#define GUI_KEY_CTRL_PLUS 0x2102  /* Ctrl+= (zoom in) */
#define GUI_KEY_CTRL_MINUS 0x2103 /* Ctrl+- (zoom out) */
#define GUI_KEY_CTRL_A    0x2104  /* Ctrl+A (select all) */
#define GUI_KEY_CTRL_L    0x2105  /* Ctrl+L (select line) */
#define GUI_KEY_CTRL_W    0x2106  /* Ctrl+W (toggle whitespace) */
#define GUI_KEY_CTRL_F    0x2107  /* Ctrl+F (find) */
#define GUI_KEY_CTRL_G    0x2108  /* Ctrl+G (go to line) */
#define GUI_KEY_CTRL_Z    0x2109  /* Ctrl+Z (undo) */
#define GUI_KEY_CTRL_Y    0x210A  /* Ctrl+Y (redo) */
#define GUI_KEY_CTRL_X    0x210B  /* Ctrl+X (cut) */
#define GUI_KEY_CTRL_C    0x2206  /* Ctrl+C (abort/copy) */
#define GUI_KEY_CTRL_V    0x2207  /* Ctrl+V (paste) */
#define GUI_KEY_CTRL_H    0x210C  /* Ctrl+H (replace/help) */
#define GUI_KEY_CTRL_N    0x210D  /* Ctrl+N (new) */
#define GUI_KEY_CTRL_D    0x210E  /* Ctrl+D (duplicate/delete) */

/* Function keys F1-F12 */
#define GUI_KEY_F1        0x5001
#define GUI_KEY_F2        0x5002
#define GUI_KEY_F3        0x5003
#define GUI_KEY_F4        0x5004
#define GUI_KEY_F5        0x5005
#define GUI_KEY_F6        0x5006
#define GUI_KEY_F7        0x5007
#define GUI_KEY_F8        0x5008
#define GUI_KEY_F9        0x5009
#define GUI_KEY_F10       0x500A
#define GUI_KEY_F11       0x500B
#define GUI_KEY_F12       0x500C

// Returns 1 if an event was written into out_event, 0 if none available, -1 on error.
int gui_poll_event(int handle, gui_event_t* out_event);
// Like poll, but blocks (hlt) until an event arrives. Returns 1 on event, -1 on error/interrupt.
int gui_wait_event(int handle, gui_event_t* out_event);