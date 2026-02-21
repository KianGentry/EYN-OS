#ifndef TILE_MANAGER_H
#define TILE_MANAGER_H

#include <misc/types.h>
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

// Find the tile index that is backed by a given vterm index.
// Returns -1 if not found.
int tile_find_by_term(int term_idx);

// Get the content rect (inside borders/title/status) in screen pixels.
// Outputs are set to 0 on failure.
void tile_get_content_rect(int tile_idx, int* out_x, int* out_y, int* out_w, int* out_h);

// Tiling mode detection
int tile_is_tiling_active();

// GUI client callback types: draw and key handler
typedef void (*tile_gui_draw_cb)(int tile_idx, int content_x, int content_y, int content_w, int content_h, void* userdata);
typedef void (*tile_gui_key_cb)(int tile_idx, int key, void* userdata);
// Optional mouse event callback (called once per event snapshot)
typedef void (*tile_gui_mouse_cb)(int tile_idx, const mouse_event_t* me, void* userdata);

// Optional close request callback (close button, Super+Q, default Ctrl+X close).
// Return 1 to allow close, 0 to veto.
typedef int (*tile_gui_close_cb)(int tile_idx, void* userdata);

// Register/unregister a GUI client for a tile (tile takes ownership until unregistered)
void tile_register_gui_client(int tile_idx, tile_gui_draw_cb draw_cb, tile_gui_key_cb key_cb, void* userdata);
// Extended registration with optional mouse callback
void tile_register_gui_client2(int tile_idx, tile_gui_draw_cb draw_cb, tile_gui_key_cb key_cb, tile_gui_mouse_cb mouse_cb, void* userdata);
// Optional: register a close request callback for this GUI tile
void tile_register_gui_close_cb(int tile_idx, tile_gui_close_cb close_cb);
void tile_unregister_gui_client(int tile_idx);

// Mark a GUI client dirty so the tiler redraws it next frame
void tile_invalidate_gui(int tile_idx);

// When enabled, the tiler will redraw this GUI tile every frame (subject to global FPS cap).
// Useful for animation/video playback without needing to spam invalidations.
void tile_set_gui_continuous_redraw(int tile_idx, int enabled);

// Render a single tiler frame immediately.
// Useful when the main tiler loop is blocked (e.g. while running a ring3 task)
// but we still want vterm output to appear on-screen.
void tile_render_once(void);

// Poll and process at most one pending UI key event.
// Returns 1 if a key was processed, 0 if no input was available.
int tile_pump_input_once(void);

void start_tiling_manager();

//  Floating window manager (experimental) 
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

// When enabled, the tiler will redraw this window every frame (subject to global FPS cap).
void wm_set_continuous_redraw(int win_id, int enabled);

// Close the window
void wm_close_window(int win_id);

//  Runtime GUI tuning (low-spec controls) 
// Mode: 0=high (full features), 1=low (wireframe drag, simplified decor), 2=auto (based on RAM)
void tiler_gui_set_mode(int mode);
// FPS cap: 0 disables the cap (unlimited), otherwise set to desired frames per second (e.g., 20/30/60)
void tiler_gui_set_fps_cap(int fps);
// Drag throttle in milliseconds (0 means minimal throttling); larger values reduce redraw rate during drags
void tiler_gui_set_drag_throttle_ms(int ms);
// Print current GUI tuning status to the shell
void tiler_gui_print_status(void);

//  Terminal background images (per tile) 
// Begin an interactive prompt to set a REI image as the background for a tile's
// content area. A popup will ask for Tile/Scale/Center (if the image is larger than
// the screen, only Scale will be offered). The function takes ownership of the image
// memory; it will be freed automatically on cancel or when the background is replaced.
// Returns 0 on success (prompt shown or background applied), -1 on failure (tiler inactive or bad params).
int tile_begin_set_background_from_rei(int tile_idx, rei_image_t* image);

// Clear any configured background image for the given tile (no-op if none).
void tile_clear_background(int tile_idx);

// --- Theme (window/tile chrome) ---
typedef struct {
	uint8 title_focused_r, title_focused_g, title_focused_b;
	uint8 title_unfocused_r, title_unfocused_g, title_unfocused_b;
	uint8 status_r, status_g, status_b;
	uint8 status_text_r, status_text_g, status_text_b;
} wm_theme_t;

// Get/set runtime theme colors used by the tiler/window manager.
void wm_theme_get(wm_theme_t* out);
void wm_theme_set(const wm_theme_t* in);
void wm_theme_reset_defaults(void);

#endif // TILE_MANAGER_H
