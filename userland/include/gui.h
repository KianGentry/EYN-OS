#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Minimal GUI/Tiling syscalls.
// Handles are small ints. Handle 0 refers to the calling task's existing tile.

int gui_create(const char* title, const char* status_left);
int gui_set_title(int handle, const char* title);

// Attach a GUI client to the calling task's current tile (handle 0).
// This avoids creating a new tile, which is important while ring3 is active
// because mouse-based focus switching isn't pumped.
int gui_attach(const char* title, const char* status_left);

// ---------------- Immediate-mode user GUI ----------------

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
};

int gui_begin(int handle);
int gui_clear(int handle, const gui_rgb_t* rgb);
int gui_fill_rect(int handle, const gui_rect_t* rect);
int gui_draw_text(int handle, const gui_text_t* cmd);
int gui_present(int handle);

// Returns 1 if an event was written into out_event, 0 if none available, -1 on error.
int gui_poll_event(int handle, gui_event_t* out_event);
// Like poll, but blocks (hlt) until an event arrives. Returns 1 on event, -1 on error/interrupt.
int gui_wait_event(int handle, gui_event_t* out_event);

#ifdef __cplusplus
}
#endif
