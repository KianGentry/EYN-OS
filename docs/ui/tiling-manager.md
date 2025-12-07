# Tiling Manager and Floating Windows

The EYN-OS GUI layer now includes a tiling front-end and an experimental floating window manager. This page describes the architecture and APIs for building GUI apps that render inside tiles or windows.

## Overview

### Beginner's Guide: The Screen Layout
EYN-OS uses a "Tiling" window manager. This means windows don't overlap randomly like papers on a messy desk. Instead, they are arranged in a neat grid, like tiles on a floor.
- **Tiles**: The main work areas. They split the screen evenly.
- **Floating Windows**: Special windows (like pop-ups) that sit *on top* of the tiles.
- **Virtual Terminals**: Each tile acts like its own separate computer screen.

### Screen Layout Diagram
```
┌───────────────────────────────┬───────────────────────────────┐
│ Tile 1 (Top Left)             │ Tile 2 (Top Right)            │
│                               │                               │
│   ┌───────────────────────┐   │                               │
│   │ Floating Window       │   │                               │
│   │ (Sits on top)         │   │                               │
│   └───────────────────────┘   │                               │
│                               │                               │
├───────────────────────────────┼───────────────────────────────┤
│ Tile 3 (Bottom Left)          │ Tile 4 (Bottom Right)         │
│                               │                               │
│                               │                               │
│                               │                               │
│                               │                               │
│                               │                               │
└───────────────────────────────┴───────────────────────────────┘
```

## Architecture

### System Diagram
```
┌───────────────────────────────┐
│            Apps               │
│  (viewer, draw, write, help)  │ 
├───────────────────────────────┤
│  Tile/Window Manager (GUI)    │
│  (tiling_manager.c)           │
├───────────────────────────────┤
│  Virtual Terminals            │
│  (terminals.c)                │
├───────────────────────────────┤
│  VGA + Mouse Drivers          │
│  (dirty rects, cursor, wheel) │
└───────────────────────────────┘
```

Key files:
- `src/utilities/tui/tiling_manager.c`
- `src/utilities/tui/terminals.c`, `include/utilities/terminals.h`
- `include/utilities/tile_manager.h`
- `src/drivers/vga.c`, `include/drivers/vga.h`
- `src/drivers/mouse.c`, `include/drivers/mouse.h`

## Tile GUI API

Headers: `include/utilities/tile_manager.h`

```c
// Create a new GUI tile; returns tile index or -1
int tile_create_gui_tile(const char* title, const char* status_left);

// Register GUI client callbacks for a tile
typedef void (*tile_gui_draw_cb)(int tile_idx, int x, int y, int w, int h, void* ud);
typedef void (*tile_gui_key_cb)(int tile_idx, int key, void* ud);
typedef void (*tile_gui_mouse_cb)(int tile_idx, const mouse_event_t* me, void* ud);

void tile_register_gui_client2(int tile_idx,
    tile_gui_draw_cb draw,
    tile_gui_key_cb key,
    tile_gui_mouse_cb mouse,
    void* userdata);

// Title/status helpers and redraw
void tile_set_title_status(int tile_idx, const char* title, const char* left, const char* right);
void tile_invalidate_gui(int tile_idx);
void tile_invalidate_decorations(int tile_idx);
```

Behavior:
- The manager calls your draw callback with the content rectangle (excludes title/status/border)
- Call `tile_invalidate_gui()` when your app state changes to request a redraw
- Keyboard and mouse callbacks receive events only when the tile is focused

## Floating Window API (experimental)

```c
// Create a window; returns id or -1
int wm_create_window(const char* title, int x, int y, int w, int h, const char* status_left);

// Register callbacks
void wm_register_gui_client2(int win_id,
    tile_gui_draw_cb draw,
    tile_gui_key_cb key,
    tile_gui_mouse_cb mouse,
    void* userdata);

// Update decorations and request redraws
void wm_set_title_status(int win_id, const char* title, const char* left, const char* right);
void wm_invalidate_window(int win_id);
void wm_close_window(int win_id);
```

Windows include title bars with minimize, maximize, and close buttons, with focused/unfocused icon variants loaded from REI assets (`testdir/ui/*.rei`).

## Virtual Terminals

Headers: `include/utilities/terminals.h`

- One vterm per tile (80×N), with scrollback
- Mouse wheel maps to scrollback when no GUI client is registered on a tile
- Selection visuals on the input line are supported

Key APIs:
```c
void vterm_handle_key(int idx, int key);
void vterm_set_scroll(int idx, int scroll);
int  vterm_get_version(int idx); // content version for incremental redraw
```

## Commands

The following commands integrate with the GUI layer:
- `tiling` — launch the tiling manager UI
- `view <file.rei>` — open an image viewer in a tile
- `vieww <file.rei>` — open an image viewer in a floating window
- `draw` — open a simple canvas editor in a tile
- `win_test` — open a sample floating window (compositor test)

## Input

- Keyboard: routed to the focused tile/window; common shortcuts like Ctrl+X to close
- Mouse: click-to-focus, drag within content for apps that support it; wheel scrolls vterm
- Cursor: optional REI image overlay for the pointer

## Performance

- The manager uses dirty-rectangle tracking and backbuffer-aware pixel ops to reduce flicker
- GUI apps should draw only within the provided content rect and avoid full-screen clears

### Runtime GUI Tuning

For low-spec environments or profiling, the tiler exposes a few runtime knobs:

- Mode: 0=high, 1=low, 2=auto (based on detected RAM)
    - `tiler_gui_set_mode(int mode)`
- FPS cap to limit redraw rate:
    - `tiler_gui_set_fps_cap(int fps)` (0 disables)
- Drag throttle to reduce redraws while dragging windows:
    - `tiler_gui_set_drag_throttle_ms(int ms)`
- Inspect current settings:
    - `tiler_gui_print_status()`

Low mode simplifies decor and uses wireframe outlines during drags to avoid overdraw on very slow CPUs.

### Per-Tile Background Images

Tiles can render a custom REI image behind the terminal text with optional darkening and text adaptation.

Commands:
- `setbg <file.rei>` — Choose how to display (Tile/Scale/Center) and apply to the focused tile
- `clearbg` — Remove the background for the focused tile

APIs:
```c
// Begin a small chooser and take ownership of the provided image memory
int tile_begin_set_background_from_rei(int tile_idx, rei_image_t* image);
void tile_clear_background(int tile_idx);
```

## Examples

Minimal tile app:
```c
static void app_draw(int t, int x, int y, int w, int h, void* ud) {
    drawRect(x, y, w, h, 0, 0, 0);
    const char* msg = "Hello Tile";
    for (int i = 0; msg[i]; ++i) drawCharAt(x + 4 + i*8, y + 4, (unsigned char)msg[i], 255,255,0);
}

static void app_key(int t, int key, void* ud) {
    if (key == 0x2002) { /* Ctrl+X */ tile_unregister_gui_client(t); }
}

int t = tile_create_gui_tile("Demo", "Ctrl+X: Close");
tile_register_gui_client2(t, app_draw, app_key, NULL, NULL);
```

To optimize on very slow machines:
```c
tiler_gui_set_mode(2);        // auto
tiler_gui_set_fps_cap(30);    // cap redraws
tiler_gui_set_drag_throttle_ms(33); // ~30Hz drag updates
```

## Future Work

- Window movement/resizing, better stacking controls
- Extended alpha blending paths and richer widgets
- Additional input affordances (context menus, selection across lines)
