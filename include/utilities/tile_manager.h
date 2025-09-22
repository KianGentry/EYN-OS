#ifndef TILE_MANAGER_H
#define TILE_MANAGER_H

#include <types.h>

// Set GUI title/status for a given tile index (0..3).
// title: centered title text
// status_left: small text shown left in the little gray status bar below the title
// status_right: small text shown right in the same bar (e.g. "[MODIFIED]")
void tile_set_title_status(int tile_idx, const char* title, const char* status_left, const char* status_right);

// Close the specified tile (make it empty and repack tiles). If tile_idx is focused, focus will move sensibly.
void tile_close(int tile_idx);

// Get the currently focused tile index (0..tile_count-1)
int tile_get_focused();

// Tiling mode detection
int tile_is_tiling_active();

// GUI client callback types: draw and key handler
typedef void (*tile_gui_draw_cb)(int tile_idx, int content_x, int content_y, int content_w, int content_h, void* userdata);
typedef void (*tile_gui_key_cb)(int tile_idx, int key, void* userdata);

// Register/unregister a GUI client for a tile (tile takes ownership until unregistered)
void tile_register_gui_client(int tile_idx, tile_gui_draw_cb draw_cb, tile_gui_key_cb key_cb, void* userdata);
void tile_unregister_gui_client(int tile_idx);

void start_tiling_manager();

#endif // TILE_MANAGER_H
