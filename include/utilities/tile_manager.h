#ifndef TILE_MANAGER_H
#define TILE_MANAGER_H

#include <types.h>
#include <mouse.h>
#include <rei.h>

// Set GUI title/status for a given tile index (0..3).
// title: centered title text
// status_left: small text shown left in the little gray status bar below the title
// status_right: small text shown right in the same bar (e.g. "[MODIFIED]")
void tile_set_title_status(int tile_idx, const char* title, const char* status_left, const char* status_right);
// Force decoration (title/status/borders) redraw next frame without touching content
void tile_invalidate_decorations(int tile_idx);
// Create a new tile suitable for a GUI client and return its index, or -1 if no space
int tile_create_gui_tile(const char* title, const char* status_left);

// Close the specified tile (make it empty and repack tiles). If tile_idx is focused, focus will move sensibly.
void tile_close(int tile_idx);

// Get the currently focused tile index (0..tile_count-1)
int tile_get_focused();

// Tiling mode detection
int tile_is_tiling_active();

// GUI client callback types: draw and key handler
typedef void (*tile_gui_draw_cb)(int tile_idx, int content_x, int content_y, int content_w, int content_h, void* userdata);
typedef void (*tile_gui_key_cb)(int tile_idx, int key, void* userdata);
// Optional mouse event callback (called once per event snapshot)
typedef void (*tile_gui_mouse_cb)(int tile_idx, const mouse_event_t* me, void* userdata);

// Register/unregister a GUI client for a tile (tile takes ownership until unregistered)
void tile_register_gui_client(int tile_idx, tile_gui_draw_cb draw_cb, tile_gui_key_cb key_cb, void* userdata);
// Extended registration with optional mouse callback
void tile_register_gui_client2(int tile_idx, tile_gui_draw_cb draw_cb, tile_gui_key_cb key_cb, tile_gui_mouse_cb mouse_cb, void* userdata);
void tile_unregister_gui_client(int tile_idx);

// Mark a GUI client dirty so the tiler redraws it next frame
void tile_invalidate_gui(int tile_idx);

// Render a single tiler frame immediately.
// Useful when the main tiler loop is blocked (e.g. while running a ring3 task)
// but we still want vterm output to appear on-screen.
void tile_render_once(void);

// Poll and process at most one pending UI key event.
// Returns 1 if a key was processed, 0 if no input was available.
int tile_pump_input_once(void);

void start_tiling_manager();

// ---------------- Floating window manager (experimental) ----------------
// Windows are GUI-only (no vterm). They render and receive input on top of tiles.

// Create a floating window. Returns window id (>=0) or -1 on failure.
// Position/size are in screen pixels. Title centered; status_left optional small text.
int wm_create_window(const char* title, int x, int y, int w, int h, const char* status_left);

// Register/unregister GUI client callbacks for a window
void wm_register_gui_client2(int win_id,
	tile_gui_draw_cb draw_cb,
	tile_gui_key_cb key_cb,
	tile_gui_mouse_cb mouse_cb,
	void* userdata);
void wm_unregister_gui_client(int win_id);

// Update window title/status
void wm_set_title_status(int win_id, const char* title, const char* status_left, const char* status_right);

// Mark window content dirty to force redraw next frame
void wm_invalidate_window(int win_id);

// Close the window
void wm_close_window(int win_id);

// ---------------- Runtime GUI tuning (low-spec controls) ----------------
// Mode: 0=high (full features), 1=low (wireframe drag, simplified decor), 2=auto (based on RAM)
void tiler_gui_set_mode(int mode);
// FPS cap: 0 disables the cap (unlimited), otherwise set to desired frames per second (e.g., 20/30/60)
void tiler_gui_set_fps_cap(int fps);
// Drag throttle in milliseconds (0 means minimal throttling); larger values reduce redraw rate during drags
void tiler_gui_set_drag_throttle_ms(int ms);
// Print current GUI tuning status to the shell
void tiler_gui_print_status(void);

// ---------------- Terminal background images (per tile) ----------------
// Begin an interactive prompt to set a REI image as the background for a tile's
// content area. A popup will ask for Tile/Scale/Center (if the image is larger than
// the screen, only Scale will be offered). The function takes ownership of the image
// memory; it will be freed automatically on cancel or when the background is replaced.
// Returns 0 on success (prompt shown or background applied), -1 on failure (tiler inactive or bad params).
int tile_begin_set_background_from_rei(int tile_idx, rei_image_t* image);

// Clear any configured background image for the given tile (no-op if none).
void tile_clear_background(int tile_idx);

#endif // TILE_MANAGER_H
