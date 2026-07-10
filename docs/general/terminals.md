# EYN-OS Terminal/Shell Session Management & I/O Routing

## Summary

EYN-OS uses a **virtual terminal (vterm) system** managed by a **tiling window manager**. Multiple shell sessions are created as "tiles" (virtual windows) within the GUI, each tied to a separate virtual terminal. I/O is routed through a sophisticated redirect mechanism that allows the kernel to intercept printf output and stream it to the appropriate vterm or buffer. GUI applications take over the main output by spawning as user tasks with special I/O attachment points.

---

## 1. Shell/Terminal Management Code Locations

### Core Shell Implementation
- **[src/utilities/shell/shell.c](src/utilities/shell/shell.c)** — Main shell interpreter
  - Command parsing and execution dispatch
  - Dynamic UELF binary discovery and spawning
  - Command registry and hash table lookup
  - Auto-run support for unknown commands as executables

- **[include/utilities/shell/shell.h](include/utilities/shell/shell.h)** — Shell public API
  - `launch_shell(n)` — Initialize shell with n sessions (legacy, now managed by tiling manager)
  - `handle_shell_command()` — Execute a single shell command
  - `find_command()` — Lookup command handler by name

### Virtual Terminal System
- **[src/utilities/tui/terminals.c](src/utilities/tui/terminals.c)** — Virtual terminal management
  - `vterm_init_all()` — Initialize all 4 virtual terminals
  - `vterm_write_char()` — Write character to vterm display buffer
  - `vterm_feed_input()` — Add keyboard input to vterm
  - `vterm_handle_key()` — Process full key press (history, enter to execute)
  - `vterm_stdin_*()` — stdin buffer management for ring3 tasks
  - `vterm_get_cwd()` — Per-vterm current working directory

- **[include/utilities/terminals.h](include/utilities/terminals.h)** — Virtual terminal API
  - Terminal buffer structures (256-row history, 80 columns)
  - Input/output redirection
  - Scrollback and history navigation

### Tiling Window Manager (GUI Container)
- **[src/gui/tiling_manager.c](src/gui/tiling_manager.c)** — Main tiling manager
  - Initialization and main loop
  - Tile layout and lifecycle management
  - Super+n hotkey to create new shell tiles

- **[src/gui/gui_tiles.c](src/gui/gui_tiles.c)** — Tile lifecycle and drawing
  - `create_shell_tile()` — Create new shell within a tile
  - `tile_close()` — Close and cleanup tile
  - `layout_tiles()` — Position tiles on screen (stacking)
  - Per-tile decoration drawing (titlebar, status bar)

- **[src/gui/gui_input.c](src/gui/gui_input.c)** — Input pump and tile API
  - `tile_pump_input_once()` — Process one keyboard/mouse event (called from IRQ0)
  - `tile_render_once()` — Incremental rendering while user task runs
  - Keyboard routing to active tile or stdin buffer
  - Ctrl+C handling for task abort

- **[src/gui/gui_state.c](src/gui/gui_state.c)** — Shared GUI state and constants
  - Tile array (`tiles[MAX_TILES]` up to 4 tiles)
  - Focus tracking and tile type definitions
  - Background image management

---

## 2. How Multiple Shell Sessions Are Currently Created

### Method 1: Tiling Manager (Modern Approach)
The tiling manager supports up to **4 simultaneous shell tiles**:

```c
// From src/gui/tiling_manager.c (~line 2597)
// Super+n hotkey creates new shell:
if ((base & 0xFF) == 'n') {
    if (tile_count < 4) {
        int idx = tile_count++;
        tiles[idx].type = TILE_SHELL;
        tiles[idx].title = "EYN-OS Shell";
        tiles[idx].term_idx = idx;           // Tied to vterm 0..3
        tiles[idx].active = 1;
        vterm_clear(idx);
        vterm_set_active(idx, 1);
        vterm_print_prompt(idx);
        focused = idx;
        g_force_full_redraw = 1;
        layout_tiles();
    }
}
```

Each tile is bound to a virtual terminal index (0–3):
- **Tile 0** ↔ **vterm 0** (stdin/stdout/stderr)
- **Tile 1** ↔ **vterm 1**
- **Tile 2** ↔ **vterm 2**
- **Tile 3** ↔ **vterm 3**

### Method 2: Virtual Terminal System (Lower Level)
Four virtual terminals always exist:

```c
// From src/utilities/tui/terminals.c
static vterm_t vterms[4];  // Ring-buffered 256-row history each

vterm_init_all();          // Initialize all 4 (called once at startup)
vterm_set_active(idx, 1);  // Mark one active for input
vterm_clear(idx);          // Clear display buffer
vterm_print_prompt(idx);   // Print shell prompt
```

### Session Lifecycle
1. User presses **Super+n** (Super=Windows key)
2. Input pump detects hotkey and increments `tile_count` (max 4)
3. New tile created with fresh `vterm_idx`, bound to kernel vterm array
4. `vterm_clear()` resets display but preserves history
5. Each vterm has its own:
   - **Output buffer** (256 rows × 80 columns)
   - **Scrollback history** (same buffer with head/tail pointers)
   - **Cursor position** (cur_x, cur_y)
   - **Working directory** (cwd[128])
   - **Input line buffer** (input_buf, for command editing)
   - **Stdin buffer** (for ring3 task input)
   - **Command history** (50 commands max)
   - **Color metadata** (per-character RGB for syntax highlighting)

---

## 3. How I/O Is Routed Between Sessions

### Output Redirection System
The kernel intercepts **all printf() calls** via a global redirect mechanism:

```c
// From include/drivers/vga.h
#define SHELL_REDIRECT_BUF_SIZE 1024

extern int shell_redirect_active;           // 1 = capturing output
extern char shell_redirect_buf[SHELL_REDIRECT_BUF_SIZE];
extern int shell_redirect_pos;              // Write position
extern unsigned char shell_redirect_r/g/b[SHELL_REDIRECT_BUF_SIZE];  // Per-char color

void start_shell_redirect(void);            // Enable redirect
void stop_shell_redirect(void);             // Disable redirect
void vga_set_shell_redirect_stream_vterm(int vterm_idx);  // Stream to specific vterm
```

### Redirect Flow

**Case 1: Normal Shell Command (No Redirect)**
```
User types command → vterm_handle_key() → handle_shell_command()
                                       → printf() → VGA framebuffer (visual output)
                                       → Serial (COM1) for debug
```

**Case 2: Command Output Capture**
```
User types "ls" → vterm_handle_key() → start_shell_redirect()
                                     → handle_shell_command("ls")
                                     → printf() [captured to shell_redirect_buf]
                                     → stop_shell_redirect()
                                     → vterm_stream_redirect_char() displays in vterm
```

**Case 3: Ring3 User Task Running (GUI App)**
```
User task spawned with fd mapping → printf() in ring3 task
                                   → [captured if redirect active]
                                   → Can be streamed to vterm via IRQ0 pump
                                   → OR routed to GUI app's own output buffer
```

### Key Printf Interception Points

**[src/drivers/vga.c](src/drivers/vga.c)** — Printf integration (~line 1800+)
```c
// When shell_redirect_active, printf captures to shell_redirect_buf instead of screen
if (shell_redirect_active) {
    for (int i = 0; formatted_str[i]; i++) {
        if (shell_redirect_pos < SHELL_REDIRECT_BUF_SIZE) {
            shell_redirect_buf[shell_redirect_pos] = formatted_str[i];
            shell_redirect_r[shell_redirect_pos] = shell_redirect_colour_r;
            shell_redirect_g[shell_redirect_pos] = shell_redirect_colour_g;
            shell_redirect_b[shell_redirect_pos] = shell_redirect_colour_b;
            shell_redirect_pos++;
        }
    }
    // Optional: stream to vterm while capturing
    vga_set_shell_redirect_stream_vterm(vterm_idx);
} else {
    // Normal case: write directly to VGA framebuffer
    [draw to screen]
}
```

### Stdin Management for Separate Sessions

Each vterm has its own stdin buffer:

```c
// From src/utilities/tui/terminals.c
typedef struct {
    char stdin_buf[256];        // Buffered user input
    volatile int stdin_len;     // Current bytes
    volatile int stdin_ready;   // 1 when line complete (Enter pressed)
} vterm_t;

// User presses key while ring3 task running:
vterm_stdin_putchar(term_idx, ch);      // Add to stdin buffer
if (vterm_stdin_ready(term_idx)) {
    const char* data = vterm_stdin_data(term_idx);  // Read buffered line
    int len = vterm_stdin_len(term_idx);
    // Ring3 task's READ syscall consumes this
    vterm_stdin_consume(term_idx);      // Clear buffer
}
```

### Working Directory Per Session

Each vterm maintains its own cwd:

```c
// From src/utilities/tui/terminals.c
char cwd[128];  // Per-vterm current working directory

// When "cd" command executes in tile 1:
vterm_t* t = &vterms[1];
strcpy(t->cwd, "/new/path");

// Subsequent "ls" commands in tile 1 use this directory
// Tile 0 unaffected
```

---

## 4. What Causes GUI Apps to Take Over the Main Output Stream

### User Task Spawning & Ring3 Execution

When a GUI application is launched from shell:

```c
// From src/utilities/shell/shell.c (~line 179)
if (try_run_uelf_at_path(drive, "/binaries/doom", argc, argv)) {
    (void)user_task_spawn_argv(drive, "/binaries/doom", argc, argv);
    return;  // Returns immediately; task runs async
}
```

The kernel spawns a **ring3 user task** via:

```c
// From src/cpu/user_elf.c
int user_task_spawn_argv(uint8 drive, const char* path, int argc, const char* const* argv) {
    // 1. Load UELF from disk
    // 2. Set up ring3 address space
    // 3. Set g_user_task_active = 1
    // 4. Set g_user_task_term = term_index (which vterm this task is tied to)
    // 5. Return immediately; IRQ0 handles task execution
}
```

### How GUI Takes Over: The IRQ0 Pump

The **PIT timer interrupt (IRQ0)** drives all non-blocking I/O while a user task runs:

```c
// From src/cpu/irq.c (~line 185)
if (irq_number == 0 && g_user_task_active) {
    // While ring3 task runs, pump UI/input/output
    watchdog_kick("user-task");
    
    if (tile_is_tiling_active()) {
        int any_key = 0;
        for (int i = 0; i < 4; ++i) {
            if (!tile_pump_input_once()) break;  // Get one keyboard event
            any_key = 1;
        }
        if (any_key) g_user_task_ui_dirty = 1;
    }
    
    // Throttle renders (PIT is 1000 Hz, render at ~16 FPS while task active)
    static uint32 ui_div = 0;
    if (++ui_div >= 62) {  // ~16 FPS max
        ui_div = 0;
        tile_render_once();  // Render updated vterm/GUI
    }
}
```

**Key flow:**
1. **Ring3 program prints** → printf() output goes to screen/buffer
2. **IRQ0 fires** (1000 Hz) → Kernel pumps one input event + renders vterm
3. **Keyboard input captured** → Routed to ring3 task's stdin buffer or GUI callbacks
4. **Task sleeps/blocks** → Kernel continues pumping UI so desktop stays responsive
5. **Task exits/aborted** → Ctrl+C in IRQ0 sets `g_user_interrupt = 1` → task cleaned up

### GUI Registration for Custom Input Handling

GUI applications can register callbacks to intercept input:

```c
// From src/gui/gui_input.c (~line 1050)
// If a GUI app registered a key callback:
if (gui_key_cb[term]) {
    gui_key_cb[term](target_tile, key, gui_userdata[term]);
}
```

This allows text editors, games, etc. to handle keys directly without stdin buffering.

### Output Capture While Task Runs

To keep the desktop responsive while a ring3 task runs silently:

```c
// From src/drivers/vga.h
void vga_set_shell_redirect_stream_vterm(int vterm_idx);

// Ring3 task's printf is redirected+streamed to UI:
start_shell_redirect();
vga_set_shell_redirect_stream_vterm(g_user_task_term);  // Stream to task's vterm
// Ring3 task runs and calls printf()...
vga_clear_shell_redirect_stream_vterm();
stop_shell_redirect();
```

Each character output by the ring3 task is:
1. Captured to `shell_redirect_buf`
2. Streamed to the task's vterm display
3. Rendered on next IRQ0 frame in `tile_render_once()`

---

## 5. Serial Port Output (Why Separate)

### Serial Output Purpose
Serial port (COM1 @ 0x3F8) is reserved for **kernel debug and panic output**:

```c
// From src/drivers/serial.h
#define SERIAL_COM1 0x3F8

// From include/drivers/serial.h
int serial_write(uint16 port, const char* data, int length);
int serial_write_char(uint16 port, char c);
```

### Serial vs GUI Output

| Output Type | Destination | Purpose | Routing |
|---|---|---|---|
| **GUI printf()** | VGA framebuffer | User shell, normal output | `shell_redirect_buf` or direct to screen |
| **Serial printf()** | COM1 @ 0x3F8 | Kernel debug, panics | Always to serial port |
| **Panic/Backtrace** | COM1 | Emergency kernel info | Via `serial_write()` in panic handlers |
| **Ring3 stdout** | Vterm or GUI buffer | User task output | Via redirect mechanism or file descriptor |

### Serial Output Locations

```c
// From src/misc/panic.c (~line 206)
// Kernel panic: ALWAYS to serial, then halt
serial_write(SERIAL_COM1, "\nKERNEL PANIC: ", 15);
if (msg) serial_write(SERIAL_COM1, msg, (int)strlen(msg));
serial_write(SERIAL_COM1, "\n", 1);

// From src/drivers/ac97.c (~line 281)
// AC97 audio driver debug
serial_write(SERIAL_COM1, "[AC97] No AC97 controller found on PCI bus\n", 43);

// From src/drivers/vga.c (~line 1880)
// Debug printf also echoes to serial
serial_write_char(SERIAL_COM1, temp[i]);
```

**Why separate?**
- **Reliability**: Serial always works, even if GUI/VGA is broken
- **Debugging**: Debug server can read serial from QEMU/emulator
- **Panic safety**: When kernel panics, GUI is unreliable; serial provides console fallback
- **Early boot**: Before graphics initialized, kernel messages go to serial

---

## 6. Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                         EYN-OS Main Loop                        │
│                    (Tiling Manager Loop)                        │
└─────────────────────────────────────────────────────────────────┘
                                 ↑
                  ┌──────────────┴──────────────┐
                  ↓                              ↓
         ┌─────────────────┐        ┌──────────────────────┐
         │  Render Tiles   │        │  Pump Input Events   │
         │ (4 max tiles)   │        │ (Keyboard/Mouse)     │
         └─────────────────┘        └──────────────────────┘
                 ↓                              ↓
         ┌─────────────────┐        ┌──────────────────────┐
         │  Per-Tile Draw  │        │  Route to Active     │
         │ (calls vterm_*) │        │  Tile / Ring3 Task   │
         └─────────────────┘        └──────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                    IRQ0 Handler (1000 Hz)                       │
│              (Runs WHILE ring3 task is active)                  │
├─────────────────────────────────────────────────────────────────┤
│ 1. Pump input: tile_pump_input_once() x 4 (get keyboard)        │
│ 2. Route input: stdin buffer or GUI callback                    │
│ 3. Render: tile_render_once() every ~62ms (16 FPS)              │
│ 4. Output: Stream printf() to vterm via redirect                │
│ 5. Abort: Check Ctrl+C, set g_user_interrupt if pressed         │
└─────────────────────────────────────────────────────────────────┘
                                 ↓
         ┌──────────────────────────────────────────┐
         │  Virtual Terminal Array (4 max)          │
         │  vterms[0..3]                            │
         ├──────────────────────────────────────────┤
         │ vterm[0]:  output buffer,  stdin buf,    │
         │            history,       cwd            │
         │ vterm[1]:  output buffer,  stdin buf,    │
         │            history,       cwd            │
         │ ...                                      │
         └──────────────────────────────────────────┘
                  ↓                      ↓
         ┌─────────────────┐  ┌───────────────────┐
         │  VGA/Graphics   │  │ Serial COM1       │
         │  Framebuffer    │  │ (Debug/Panic)     │
         │  (Main display) │  │                   │
         └─────────────────┘  └───────────────────┘
```

---

## 7. Key Code Flow Examples

### Creating a New Shell Session (Super+n)

```
User presses Super+n
  ↓
gui_input.c: tile_pump_input_once()
  ↓ detects Super+n (key == 'n' | 0x4000)
  ↓
tiling_manager.c: Create new tile
  - tiles[tile_count].type = TILE_SHELL
  - tiles[tile_count].term_idx = tile_count  (0..3)
  - tile_count++
  ↓
vterm_clear(new_idx)
  - Reset output buffer
  - Reset cursor to (0, 0)
  ↓
vterm_print_prompt(new_idx)
  - Prints "0:/! " to vterm display
  ↓
g_force_full_redraw = 1
  ↓
Next frame: tile_render_once() draws new tile on screen
```

### User Typing "doom" and Launching GUI App

```
User types: "doom" + Enter
  ↓
vterm_handle_key() in active tile
  ↓ Accumulates input to input_buf
  ↓ User presses Enter (key == '\n')
  ↓
handle_shell_command("doom")
  ↓ Looks up "doom" command
  ↓ Not found in hash table
  ↓
try_run_unknown_as_uelf("doom")
  ↓ find_uelf_recursive() searches /binaries/doom
  ↓ Finds /binaries/doom executable
  ↓
user_task_spawn_argv(drive, "/binaries/doom", argc, argv)
  ↓
src/cpu/user_elf.c:
  - Load ELF from disk
  - Set g_user_task_active = 1
  - Set g_user_task_term = (index of active vterm)
  - Return to shell (task runs async)
  ↓
Shell returns control to tiler loop
  ↓
Next IRQ0 (timer):
  - Check if g_user_task_active
  - Pump input: tile_pump_input_once()
  - Route input to g_user_task_term stdin buffer
  - Render: tile_render_once() shows vterm output
  - If ring3 task calls printf():
    - start_shell_redirect()
    - vga_set_shell_redirect_stream_vterm(g_user_task_term)
    - printf() captured + streamed to vterm
    - vterm_stream_redirect_char() displays in tile
```

### Command Output Capture (e.g., "ls" command)

```
User types: "ls" + Enter
  ↓
vterm_handle_key() → handle_shell_command("ls")
  ↓
start_shell_redirect()
  - shell_redirect_active = 1
  - shell_redirect_pos = 0
  ↓
Execute "ls" command (kernel shell.c:cmd_list)
  ↓
printf("  dir1/\n")
  printf("  file.txt\n")
  ↓ Each printf call:
    - Checks shell_redirect_active
    - Instead of drawing to screen:
      → Copies to shell_redirect_buf[]
      → Records color in shell_redirect_r/g/b[]
      → Increments shell_redirect_pos
  ↓
stop_shell_redirect()
  - shell_redirect_active = 0
  - captured_len = shell_redirect_pos
  ↓
Loop through shell_redirect_buf[0..captured_len]:
  for each char:
    vterm_stream_redirect_char(term_idx, char, r, g, b)
      ↓ Adds to vterm's output buffer
      ↓ Updates display
  ↓
Next frame renders updated vterm on screen
```

---

## 8. File Descriptor Management (PTY Support)

For advanced users running ptys spawned programs:

```c
// From EYN-packages/packages/ptyspawn/ptyspawn_uelf.c

int pty_fds[2] = {-1, -1};
eyn_sys_pty_open(pty_fds)  // Allocate master + slave file descriptors
  ↓
// master_fd: kernel-side for reading program output
// slave_fd:  passed to child task's stdin/stdout/stderr
  ↓
eyn_spawn_ex_req_t req;
req.stdin_fd = slave_fd;
req.stdout_fd = slave_fd;
req.stderr_fd = slave_fd;
req.path = argv[0];
  ↓
eyn_sys_spawn_ex(&req)  // Spawn with FD remapping
  ↓
// Child task's stdout/stderr writes go to slave_fd
// Master reads output and relays to parent
```

---

## Summary of Key Findings

| Component | Location | Purpose |
|---|---|---|
| **Shell Core** | `src/utilities/shell/shell.c` | Parse & execute commands |
| **Virtual Terminals** | `src/utilities/tui/terminals.c` | 4 independent display + I/O buffers |
| **Tiling Manager** | `src/gui/tiling_manager.c` | Manage & layout tiles (GUI windows) |
| **Input Pump** | `src/gui/gui_input.c` | Route keyboard to tiles or ring3 tasks |
| **Output Redirect** | `src/drivers/vga.c` | Intercept printf for capture/streaming |
| **User Task Exec** | `src/cpu/user_elf.c` | Load & run ring3 UELF programs |
| **IRQ0 Handler** | `src/cpu/irq.c` | Keep UI responsive while ring3 task runs |
| **Serial Debug** | `src/drivers/serial.c` | Panic/debug output to COM1 |
| **PTY Support** | `EYN-packages/packages/ptyspawn/` | Advanced program spawning with ptys |

**Multiple sessions achieved via:**
- 4 virtual terminal buffers (vterm[0..3])
- 1 tiling manager managing up to 4 GUI tiles
- Each tile tied to one vterm
- Independent I/O, history, and working directory per session

**GUI app takeover mechanism:**
- User spawns as async ring3 task via `user_task_spawn_argv()`
- `g_user_task_active` flag signals task is running
- IRQ0 (timer) pump keeps UI/input responsive
- Task's I/O redirected to its bound vterm's display
- Ctrl+C during task execution triggers abort
