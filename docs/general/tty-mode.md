# TTY-Only Mode

## Overview

EYN-OS now supports a **TTY-only mode**, a lightweight, text-based build configuration designed for minimal resource usage, easier debugging, and headless environments. This mode disables the graphical subsystem entirely, producing a smaller ISO and faster boot times.

## When to Use TTY Mode

- **Lightweight VMs/Machines**: Machines with very limited memory or minimal graphics capabilities
- **Headless/Remote Debugging**: Development environments without a display
- **Testing**: Faster iteration on kernel logic without graphics overhead
- **Serial Console Debugging**: Direct text output to serial ports for remote diagnostics

## Building TTY-Only ISO

### Quick Start

```bash
make iso-tty
```

This command:
1. Cleans the build directory
2. Sets `CONFIG_GUI_ENABLED=0` and `CONFIG_TTY_ENABLED=1`
3. Builds the kernel and userland without graphical components
4. Excludes TTY-incompatible packages (xeyes, gui_demo, tetris, etc.)
5. Generates `eyn-os-tty.iso`

### Building GUI+TTY ISO

```bash
make iso-gui
```

This builds a full ISO with both graphical and text modes available at boot.

### Interactive Configuration

To customize build flags, use:

```bash
make menuconfig
```

Then select **Build Flags** to toggle GUI and TTY modes.

## Boot Options

### TTY-Only Mode

When booting the TTY-only ISO:
1. VGA text mode initializes automatically (80×25 characters)
2. Kernel messages display in white text
3. Text shell launches and awaits commands
4. No graphical interface or tiling manager is available

### GUI+TTY Dual Mode

When booting the full ISO:
1. The tiling manager starts by default (graphical 4-terminal interface)
2. Press **ESC** at boot or during early startup to force text mode
3. Exit the tiling manager to return to the text shell

## VGA Text Mode Features

The TTY-only mode uses a **80×25 character VGA text mode driver** with:
- Standard ASCII character output
- Hardware cursor positioning and scrolling
- White-on-black default colors
- Automatic line wrapping and buffer scrolling
- Mirror output to serial console (COM1, 115200 baud)

## Serial Console

Both TTY and GUI modes support serial console output:

### QEMU

```bash
qemu-system-i386 -cdrom eyn-os-tty.iso -serial stdio
```

Output appears directly in the terminal.

### Hardware/VirtualBox

Configure COM1 (115200 baud, 8N1) to see kernel and shell output on an external terminal.

## Package Exclusions in TTY Mode

The following packages are **excluded** from TTY-only builds to save space:

**Graphics Programs**: xeyes, draw, rect, fontpreview, gui_demo, win_test, kwin_test  
**Theming Tools**: settings, theme, title, setfont, setbg, clearbg  
**Image Viewers**: view_backend_bmp, view_backend_builtin, view_backend_rei, view_backend_reis, view_backend_reiv  
**Games**: tetris, snake, breakout, doom, zsnes_1_51  
**Tiling Manager**: tiling

## Size Comparison

Typical ISO sizes:

| Build | Size (approx) | Notes |
|-------|---------------|-------|
| TTY-only | ~2-3 MB | Minimal; no graphics |
| GUI+TTY | ~4-5 MB | Full with graphical UI |

## Keyboard Shortcuts in TTY Mode

| Key | Action |
|-----|--------|
| Ctrl+C | Interrupt current command |
| Ctrl+D | Send EOF (exit shell) |
| Tab | Command completion (if implemented) |
| Up/Down Arrows | Command history (if implemented) |

## Troubleshooting

### No Text Output

If booting TTY-only ISO but seeing no output:
1. Check VGA text mode initialization in kernel log (serial console)
2. Verify the ISO was built with `CONFIG_TTY_ENABLED=1`
3. Test serial console: `qemu -serial stdio`

### Mixed Output in Dual Mode

If both tiling manager and text shell interfere:
1. Use Ctrl+C in the tiling manager to signal interrupt
2. Type `exit` in the shell to cleanly exit back to tiling

### Package Not Found

If a CLI tool is missing from TTY-only build:
1. It may have been excluded due to GUI dependencies
2. Rebuild with `make iso-gui` to include optional tools
3. Or add the tool to `TTY_EXCLUDES` exceptions in the Makefile

## Build Configuration Details

### Menuconfig "Build Flags" Section

The **Build Flags** menu in `make menuconfig` offers:

- **Enable graphical subsystem (GUI)**: Toggle inclusion of X11, tiling manager, graphics drivers
- **Enable text mode shell (TTY)**: Toggle the classic text-based shell
- At least one mode must be enabled

### Compiler Flags

TTY/GUI configuration is passed to all builds via:

```makefile
-DCONFIG_GUI_ENABLED=0 -DCONFIG_TTY_ENABLED=1
```

Kernel and driver code can use preprocessor conditionals:

```c
#if CONFIG_GUI_ENABLED
    // Graphical initialization
#endif

#if CONFIG_TTY_ENABLED
    vga_text_init();  // Text mode setup
#endif
```

## Development Notes

### Modifying TTY Exclusions

Edit `Makefile` in the root directory:

```makefile
TTY_EXCLUDES := xeyes draw rect fontpreview gui_demo ...
```

The `EYN-packages/Makefile` respects this list and skips building excluded packages when `CONFIG_GUI_ENABLED=0`.

### Adding TTY Support to New Code

When adding kernel code that may run in TTY mode:

1. Wrap graphics-specific code with `#if CONFIG_GUI_ENABLED`
2. For text output, use `printf()` which automatically routes to both text mode and serial
3. Avoid hardcoded framebuffer access; use the abstraction layer

### VGA Text Mode Driver

The `src/drivers/vga_text.c` driver provides:

```c
void vga_text_init(void);           // Initialize 80x25 mode
void vga_text_putchar(char c);      // Output one character
void vga_text_puts(const char *str); // Output string
void vga_text_set_color(uint8 fg, uint8 bg); // Set colors
void vga_text_clear(void);          // Clear screen
```

## See Also

- [Menuconfig Documentation](../../devtools/menuconfig.py)
- [Kernel Boot Process](./general/component-reference.md)
- [Serial Console Debugging](./debugging.md)
- [Shell Commands](./ui/shell.md)
