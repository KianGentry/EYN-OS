# X11 Compatibility Layer

EYN-OS ships a **source-level Xlib compatibility shim** that lets unmodified
X11 C programs compile and run on the native EYN-OS GUI.  The programs do not
need to link against a real X server — all Xlib calls are translated at compile
time into EYN-OS syscalls.

## Architecture

```
 ┌──────────────────────────────┐
 │  Unmodified X11 C source     │
 │  (#include <X11/Xlib.h>)     │
 └──────────┬───────────────────┘
            │ compile against userland/include/X11/*
            ▼
 ┌──────────────────────────────┐
 │  userland/libc/x11.c         │
 │  (Xlib implementation)       │
 │                              │
 │  • Software RGB565 framebuf  │
 │  • Drawing primitives        │
 │  • Event translation         │
 └──────────┬───────────────────┘
            │ int 0x80 syscalls
            ▼
 ┌──────────────────────────────┐
 │  EYN-OS kernel GUI           │
 │  gui_create / gui_blit_rgb565│
 │  gui_begin / gui_present     │
 └──────────────────────────────┘
```

The shim maintains a **userland RGB565 framebuffer** (`320 × 200` max) and
draws into it using software rasterisation.  On `XFlush()` / `XSync()` the
buffer is pushed to the kernel via `gui_blit_rgb565` (syscall 24).

## Current Status

The X11 layer is best understood as a **source-compatibility layer**, not a
full X server implementation.

- Programs still run as normal EYN-OS userland binaries.
- There is no external display server process and no X11 socket protocol.
- Compatibility is focused on small, self-contained Xlib programs that draw
   directly into a single top-level window.
- The current reference port is `xeyes`, which builds against the same headers
   and APIs that a small Xlib desktop demo would use on Unix.

This makes the layer useful for portability experiments, GUI demos, and small
legacy applications that only depend on core Xlib drawing and events.

## Constraints

| Constraint | Limit | Reason |
|---|---|---|
| Max framebuffer | 320 × 200 px | `gui_blit_rgb565` kernel limit |
| Colour depth | 16-bit RGB565 | Blit surface format |
| Windows per program | 1 | Single gui handle per process |
| Font | Fixed 8 × 8 bitmap | Built-in glyphs (ASCII 32–126) |
| GL / GLX | Not supported | No GPU pipeline |
| Xt / Motif / GTK | Not supported | Pure Xlib only |
| Pixmaps | Stubs (no-op) | No off-screen surfaces |
| Clipboard / selections | Stubs | No IPC for selections |
| Multiple screens | 1 screen | Single display |

Programs that use only core Xlib drawing (lines, rectangles, arcs, polygons,
strings) and basic event handling (keyboard, mouse, close) will work.

## What "X Windows compatibility" means in EYN-OS

EYN-OS does **not** try to emulate the complete X Window System stack.
Compatibility here means:

1. Existing C sources that include `X11/Xlib.h` can often be rebuilt with
   little or no source change.
2. The same code calls into `userland/libc/x11.c` instead of `libX11.so`.
3. Those calls are translated into EYN-OS GUI syscalls and software rendering.

In practice, this is enough for classic teaching demos, small games, and
simple desktop toys, but not for toolkit-heavy applications.

## Supported Xlib Functions

### Connection & Display
`XOpenDisplay`, `XCloseDisplay`, `XDefaultScreen`, `XDefaultVisual`,
`XDefaultColourmap`, `XDisplayWidth`, `XDisplayHeight`, `XDefaultDepth`,
`ConnectionNumber`, `XServerVendor`, `XVendorRelease`, `XProtocolVersion`

### Window Management
`XCreateSimpleWindow`, `XCreateWindow`, `XDestroyWindow`, `XMapWindow`,
`XUnmapWindow`, `XStoreName`, `XSelectInput`, `XGetWindowAttributes`,
`XGetGeometry`, `XMoveResizeWindow`, `XRaiseWindow`, `XLowerWindow`

### Graphics Context
`XCreateGC`, `XFreeGC`, `XSetForeground`, `XSetBackground`, `XSetFunction`,
`XSetLineAttributes`, `XChangeGC`

### Drawing
`XClearWindow`, `XDrawPoint`, `XDrawLine`, `XDrawRectangle`,
`XFillRectangle`, `XDrawArc`, `XFillArc`, `XFillPolygon`, `XDrawString`,
`XDrawImageString`

### Events
`XNextEvent`, `XPeekEvent`, `XPending`, `XCheckTypedEvent`, `XLookupString`,
`XCheckMaskEvent`, `XCheckTypedWindowEvent`, `XCheckWindowEvent`

### Colour
`XAllocColour`, `XParseColour`, `XAllocNamedColour`, `XLookupColour`

### Miscellaneous
`XFlush`, `XSync`, `XQueryPointer`, `XInternAtom`, `XSetWMProtocols`,
`XSetWMNormalHints`, `XSetWMProperties`, `XSetClassHint`

### Stubs (accept calls, do nothing)
`XSetErrorHandler`, `XSetIOErrorHandler`, `XFreePixmap`, `XCreatePixmap`,
`XCopyArea`, `XSetClipMask`, `XSetClipOrigin`, `XSetFillStyle`,
`XSetGraphicsExposures`, `XDefineCursor`, `XCreateFontCursor`,
`XFreeCursor`, `XGrabPointer`, `XUngrabPointer`

## Headers Provided

| Header | Contents |
|---|---|
| `X11/X.h` | Protocol constants (event types, masks, GC values) |
| `X11/Xlib.h` | Types, structs, macros, function prototypes |
| `X11/Xutil.h` | ICCCM utility types (`XSizeHints`, `XWMHints`, …) |
| `X11/Xatom.h` | Predefined atom constants (`XA_PRIMARY` … `XA_LAST_PREDEFINED`) |
| `X11/keysym.h` | Key symbol definitions (`XK_*`) |
| `X11/keysymdef.h` | Full keysym constant list |
| `X11/Xos.h` | Minimal OS-portability stub |

All headers live under `userland/include/X11/`.

## Building an X11 Program

Use the standard userland build script — the X11 shim is compiled and archived
into `libeync.a` automatically:

```bash
# GCC build
bash devtools/build_user_c.sh testdir/code/myapp_uelf.c testdir/binaries/myapp

# chibicc build
bash devtools/build_user_c_chibicc.sh testdir/code/myapp_uelf.c testdir/binaries/myapp.uelf
```

The program source should `#include <X11/Xlib.h>` (and any other X11 headers
it needs) — the build system's `-I userland/include` flag resolves them.

### Example: xeyes

A pure-Xlib `xeyes` clone is included as a reference program:

- Source: `testdir/code/xeyes_uelf.c`
- Binary: `testdir/binaries/xeyes`

Build and run:

```bash
make eynfsimg   # pack into disk image
make run        # boot QEMU
# In EYN-OS shell (GUI mode):
xeyes
```

## Colour Handling

Colours passed to `XAllocColour` use the X11 convention of 16-bit per-channel
values (`0–65535`) which are internally down-converted to RGB565.

`XParseColour` supports:
- Hex: `#RGB`, `#RRGGBB`, `#RRRRGGGGBBBB`
- ~40 named colours (`red`, `blue`, `white`, `coral`, `dodgerblue`, …)

## Event Model

| X11 event | Source |
|---|---|
| `KeyPress` | `GUI_EVENT_KEY` from kernel |
| `MotionNotify` | `GUI_EVENT_MOUSE` position fields |
| `ButtonPress` / `ButtonRelease` | `GUI_EVENT_MOUSE` button-state edge detection |
| `ClientMessage` (`WM_DELETE_WINDOW`) | `GUI_EVENT_CLOSE` from kernel |
| `Expose` | Synthesised on first `XNextEvent` call |

Keyboard scancodes are translated to X11 keycodes; `XLookupString` provides
ASCII translation for printable keys.

## Math Library

The X11 compatibility layer also introduced `userland/include/math.h`, which
provides standard math functions (`sqrt`, `sin`, `cos`, `atan2`, `pow`,
`floor`, `ceil`, etc.) implemented via inline x87 FPU assembly.  This header
is available to all userland programs, not just X11 ones.

## Porting Tips

1. **Strip toolkit dependencies** — the shim only covers core Xlib.  Remove
   Xt, Motif, GTK includes and widget calls.
2. **Keep window size ≤ 320 × 200** — larger windows will be clipped.
3. **No OpenGL** — replace GLX rendering with Xlib drawing primitives.
4. **Memory budget** — userland heap is 1 MB with a bump allocator (`free` is
   a no-op).  Avoid programs that allocate heavily at runtime.
5. **Single window** — multi-window programs need restructuring to use one
   top-level window.
