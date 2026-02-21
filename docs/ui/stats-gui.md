# Graphical System Stats (stats)

The `stats` command opens a graphical monitor that displays CPU, memory, and disk utilization using pie charts and a sortable data table.

## Features

- CPU usage estimation from scheduler tick accounting (busy vs idle hlt)
- Optional CPU MHz estimation when TSC is available
- Memory usage from heap statistics and total RAM reported by multiboot
- Disk usage from EYNFS superblock and free block bitmap
- Sortable table (click column headers) for CPU/Mem/Disk
- Efficient rendering tuned for the tiling/window manager (dirty rects)

## Usage

```bash
stats
```

Controls:
- Arrow keys and mouse for interactions within the tile
- Click column headers to change sort column/direction

## Implementation Details

Source: `src/utilities/shell/stats_gui.c`

Key techniques:
- Minimal math helpers (e.g., approximate atan2) to avoid libm
- Sampling windows and throttling to balance accuracy and performance
- Avoids 64-bit division by using cycles-per-tick estimates where needed

## Integration

The app integrates with the tiling manager via the GUI client API:

```c
static void stats_gui_draw(int tile, int x, int y, int w, int h, void* ud);
static void stats_gui_key(int tile, int key, void* ud);
static void stats_gui_mouse(int tile, const mouse_event_t* me, void* ud);

int tile = tile_create_gui_tile("System Stats", "Click headers to sort");
tile_register_gui_client2(tile, stats_gui_draw, stats_gui_key, stats_gui_mouse, NULL);
```

The command handler is registered as a streaming command (`CMD_STREAMING`) and can be unloaded if memory is tight.
