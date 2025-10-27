# What's New on dev (since origin/main)

Date: 2025-10-27

This document summarizes notable changes on the dev branch compared to origin/main, following the style of the existing EYN-OS docs. It highlights UI additions, new commands, graphics improvements, input/mouse updates, codebase refactors, build tweaks, and developer APIs.

## Highlights

- Tiling front-end manager and experimental floating window manager
- GUI API for applications (tiles and floating windows)
- Virtual terminals per tile with selection and mouse wheel scrollback
- GUI versions of commands: write editor and help system
- New GUI commands: `draw`, `view`, `vieww`, and `win_test`
- New GUI command: `stats` — graphical CPU/memory/disk monitor
- REI: RGBA support with alpha respected in compositor overlays
- VGA driver: dirty-rectangle redraws and backbuffer-aware pixel ops to reduce flicker
- Mouse driver: wheel events, improved cursor overlay, optional custom cursor image
- Watchdog timer integrated with shell and GUI loops; panic screen overhauled with stop codes
- Header reorganization into cpu/, drivers/, misc/, utilities/
- Assembler moved to utilities/assembler; docs expanded
- Makefile portability updates (Fedora grub2-mkrescue)

---

## User Interface

### Tiling Manager and Floating Windows

Files:
- `src/utilities/tui/tiling_manager.c`
- `include/utilities/tile_manager.h`
- `src/utilities/tui/terminals.c`, `include/utilities/terminals.h`
- `src/utilities/tui/tui.c`, `src/drivers/vga.c`, `src/drivers/mouse.c`

Features:
- Up to 4 tiles composing a grid terminal UI with per-tile titles/status bars
- Per-tile virtual terminal (80×N) with cursor tracking, scrollback, and incremental redraw
- Mouse integration:
  - Click-to-focus tiles and drag inside GUI clients
  - Mouse wheel scrollback for terminals (when no GUI client is registered)
  - Optional custom cursor image loaded from EYNFS (`testdir/ui/cursor.rei`)
- Experimental floating windows with title/status bars and buttons (min, max, close)
  - Per-window GUI client callbacks for draw, key, and mouse
  - Z-order, focus highlight, and unfocused icon variants for buttons
- Reduced flicker via dirty-rectangle rendering and backbuffer-aware drawing
- Runtime tuning for low-spec systems (mode, FPS cap, drag throttle)
- Per-tile background images with REI chooser and darkening

CLI entry points:
- `tiling` — Launch the tiling front-end manager (`src/utilities/shell/tiling_cmd.c`)
- `win_test` — Open a sample floating window for compositor testing

Developer API (excerpt):
```c
// Create a GUI tile and register callbacks
int t = tile_create_gui_tile("My App", "Ctrl+X: Close");
void my_draw(int t, int x, int y, int w, int h, void* ud);
void my_key(int t, int key, void* ud);
tile_register_gui_client2(t, my_draw, my_key, /*mouse*/NULL, /*userdata*/NULL);

// Floating window variant
int wid = wm_create_window("My Window", 40, 40, 360, 240, "Status left");
wm_register_gui_client2(wid, my_draw, my_key, /*mouse*/NULL, NULL);
```

See also: `docs/ui/tiling-manager.md` for architecture, API reference, and examples.

### Enhanced Help System (GUI)

Files:
- `src/utilities/shell/help_tui.c`
- `docs/ui/enhanced-help-system.md`

Features:
- Collapsible sub-command exploration within the dual-pane help UI
- Asterisk marker on commands with sub-commands; inline expansion/collapse
- Proper status bar layout and improved selection handling

### GUI Write Editor

Files:
- `src/utilities/shell/write_editor.c`

Features:
- GUI integration with the tiling manager
- Reduced flicker via incremental redraws
- Standard shortcuts preserved (Ctrl+O save, Ctrl+X exit)

### GUI Image Viewer and Simple Drawer

Files:
- `src/utilities/shell/image_viewer_gui.c` (viewer)
- `src/utilities/shell/draw_gui.c` (simple editor)

Commands:
- `view <file.rei>` — Open REI in a GUI tile
- `vieww <file.rei>` — Open REI in a floating window
- `draw` — Create/edit a simple RGB canvas with palette, zoom, brush size

Features:
- Viewer: zoom (Ctrl+Plus/Minus), pan via drag, status hints
- Drawer: color palette, brush size adjustments, basic save/quit flow

---

## Graphics and Image Support

### VGA Driver

Files: `src/drivers/vga.c`, `include/drivers/vga.h`

- Dirty-rect API and backbuffer-aware pixel ops minimize redraw cost:
  - `vga_mark_dirty_rect(x,y,w,h)`
  - `vga_drawPixel_bb(x,y,r,g,b)`
- Significantly reduces UI flicker when updating tiles and windows

### REI Image Format

Files: `src/drivers/rei.c`, `devtools/png_to_rei.py`, `docs/filesystems/rei-format.md`

- Adds RGBA decoding; alpha respected in compositor/titlebar icons
- `png_to_rei.py` supports `-d 4` depth and improved options
- New UI icon assets (`testdir/ui/*.rei`) including focused/unfocused variants

---

## Input and Mouse

Files: `src/drivers/mouse.c`, `include/drivers/mouse.h`, `src/utilities/tui/tiling_manager.c`

- Mouse event struct with deltas, wheel, and button change tracking
- Cursor overlay with save-under restore and optional REI image
- Terminal scrollback bound to mouse wheel when tile has no GUI client

---

## Diagnostics and Reliability

### Watchdog

Files: `src/misc/watchdog.c`, `include/misc/watchdog.h`

- Detects stalls based on scheduler tick; can be tuned at runtime
- Components call `watchdog_kick()` to record forward progress
- Trips into a panic with source and elapsed ticks when starved

### Panic Screen

Files: `src/misc/panic.c`, `include/misc/panic.h`, `docs/stop-codes.md`

- Blue screen of diagnostics with reason, location, category, and stop code
- Serial backtrace for debugging; see docs for interpreting stop codes

---

## Virtual Terminals

Files: `src/utilities/tui/terminals.c`, `include/utilities/terminals.h`

- Multiple vterms, one per tile, with APIs:
  - Editing, history navigation, prompt printing
  - Scrollback management (`vterm_set_scroll/get_scroll`)
  - Content versioning (`vterm_get_version`) for incremental redraw
  - Selection helpers for input-line selection highlighting

---

## Commands and Tooling

- Assembler moved to `src/utilities/assembler/` (`assemble.c`, `instruction_set.c`)
- `docs/tools/assembler.md` expanded accordingly
- `devtools/png_to_rei.py` strengthened; supports RGBA and test patterns

---

## Codebase Layout Refactor

Headers reorganized for clarity:
- `include/cpu/` — IDT, IRQ, ISR
- `include/drivers/` — VGA, keyboard, mouse, serial, EYNFS/REI, etc.
- `include/misc/` — types, kernel API, math, multiboot, sched, tui
- `include/utilities/` — string, util, paging, zero_copy, assembler, shell/*, tiler APIs

Notes:
- Includes updated across the tree; prefer new subdirectory layout
- `include/vga.h` deprecated; use `include/drivers/vga.h`
- Game engine removed (header and source)

---

## Build System

Files: `Makefile`

- Portability improvements for Fedora-based toolchains
- Uses `grub2-mkrescue` when appropriate

---

## Test Assets and Samples

- New REI assets for UI testing: `testdir/ui/{close,max,min,cursor}*.rei`
- Additional assembly sample: `testdir/calculator.asm`

---

## Migration Notes

- Update includes to new header layout
- If you reference VGA APIs directly, include `<drivers/vga.h>`
- For GUI apps, prefer the tiling/window manager registration APIs (see UI section)
- REI RGBA: alpha now respected by compositor; ensure assets include proper alpha
- `read` no longer opens images; use `view` or `vieww` to display `.rei` files

---

## Next Steps

- Expand floating window features (resize, move, stacking controls)
- Broaden alpha handling across all render paths
- Continue flicker reduction and performance profiling
