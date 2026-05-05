# EYN-OS - a public domain x86 operating system

EYN-OS is a small, educational operating system for 32‑bit x86 built entirely from scratch. It aims to be clear, portable, and approachable, favoring simple designs that are easy to learn from and extend.

## What it provides

- A freestanding 32‑bit kernel with a straightforward boot path (Multiboot‑compatible via GRUB)
- Hardware drivers: VGA display, PS/2 keyboard, ATA/IDE storage, Intel e1000 NIC, serial output
- Networking: UDP/IPv4 stack with ARP support for basic network communication
- Ring‑3 userspace: proper privilege separation with syscall interface (int 0x80)
- Native EYNFS filesystem with MBR partitioning support, plus FAT32 compatibility
- Text‑based shell and TUI framework with customizable tiling window manager
- Source-level X11/Xlib compatibility layer for small GUI ports (`xeyes`, test apps)
- Bitmap font loading (`.hex` format, 8×8 or 8×16) with runtime system-font switching
- Built‑in C compiler (chibicc) for native development and userland ELF programs (`.uelf`)
- Native DOOM support: prebuilt host-cross-compiled port plus in-OS `build_doom` script using chibicc
- Developer tools: assembler, linker, program loader, debugger facilities
- Robustness features: watchdog timer, panic recovery, memory protection
- REIV video format support for playing MP4/GIF animations in the viewer
- Alias system for custom shell commands with parameter substitution
- Strong emphasis on portability and low‑memory operation (runs on 9MB RAM)

Rather than relying on external libraries, EYN-OS includes minimal, well‑documented implementations of core facilities such as memory management, interrupts/exceptions, device I/O, networking, and graphics/text rendering.

## Architecture and philosophy

EYN-OS follows a "learnable core" approach:
- From‑scratch components with small, readable implementations (no libc)
- Clear layering: CPU/interrupts → drivers → kernel utilities → UI/tools
- Conservative memory footprint suitable for very low‑RAM targets
- Deterministic behavior prioritized over feature breadth

### What makes it different
- Education first: code explains the "why," not just the "what"
- Minimal dependencies: predictable builds and easy portability
- Tight scope: only the primitives needed to understand an OS stack
- Practical docs: APIs and internals documented alongside the code

## Architecture at a glance

- **Target:** 32‑bit x86, Multiboot‑compatible boot (via GRUB)
- **Memory:** Flat memory model with paging support, compact heap with corruption detection
- **Drivers:** VGA display, PS/2 keyboard/mouse, ATA/IDE storage, Intel e1000 NIC, serial debug
- **Networking:** UDP/IPv4 stack with ARP, basic socket-like interface for userland
- **Filesystems:** Native EYNFS with MBR partitioning, FAT32 read/write support
- **Userspace:** Ring-3 privilege separation with syscall API (int 0x80), ELF32-based UELF format
- **Compiler:** Integrated chibicc C compiler for on-system development
- **Compatibility:** X11/Xlib shim for source-level ports to the native GUI
- **UI:** Shell with command streaming, customizable tiling manager, bitmap font rendering (8×8, 8×16)
- **Applications:** Native GUI apps, X11 demo apps, and a userland DOOM port
- **Robustness:** Watchdog timer for hang detection, panic recovery, memory integrity checks

The codebase is organized by domain (CPU, drivers, misc, utilities, network) with public headers under `include/` and implementation in `src/`.

## Getting started

Build from a Linux host with standard toolchain components (GCC, NASM, GRUB):

```bash
make menuconfig # configure architecture and installer payload options
make build      # build ISO and disk images
make run        # build and run in QEMU
make qemu-gdb   # launch with GDB support (halted at startup, attach to :1234)
```

Inside EYN-OS, try these commands:
```bash
init            # initialize ATA drives
ls              # list files
help            # interactive help system
e1000 init      # initialize network (if e1000 NIC present)
chibicc --help  # C compiler help (chibicc.uelf needs to be in root)
xeyes           # run the X11 compatibility demo
run hello.uelf  # run a userland program
```

`run` is the normal launcher for ELF binaries and scripts. `ldso` is the dynamic loader used when an ELF binary declares an interpreter (`PT_INTERP`), so you usually run programs with `run` instead of invoking `ldso` directly.

To try it on real hardware, write `EYNOS.iso` to a USB drive or CD and boot on a 32‑bit compatible machine.

## System requirements

- 32‑bit x86 CPU (i386 or higher)
- ~9 MB RAM minimum (QEMU default configuration)
- VGA‑compatible display
- PS/2 keyboard (mouse optional but supported)
- ATA/IDE storage device
- VGA capable graphics processor
- (Optional) Intel e1000-compatible NIC for networking

## Documentation

The `docs/` directory contains comprehensive project documentation:

- **[docs/README.md](docs/README.md)** - Documentation index and navigation
- **[docs/system-overview.md](docs/system-overview.md)** - Architecture and design philosophy
- **[docs/QUICK-DEBUG.md](docs/QUICK-DEBUG.md)** - One-page debugging reference (keep this open!)
- **[docs/command-reference.md](docs/command-reference.md)** - Complete shell command reference (auto-generated)
- **[docs/quick-reference.md](docs/quick-reference.md)** - Quick command cheat sheet
- **[docs/stop-codes.md](docs/stop-codes.md)** - Panic stop code reference

Key topics:
- **Debugging:** Quick debug card, stop codes, GDB integration, serial logging
- **UI & Shell:** Tiling manager, TUI system, shell scripting, help system
- **GUI Compatibility:** X11/Xlib source-compatibility layer for small ports
- **Filesystems:** EYNFS specification, FAT32 support, partitioning
- **Development:** Assembler, chibicc compiler, UELF format, syscalls, in-OS builds
- **Applications:** DOOM port, Second Reality demo, GUI utilities
- **Networking:** e1000 driver, UDP/IPv4 stack (docs/network/)
- **Memory:** Paging, virtual memory, heap management
- **Hardware:** Watchdog timer, PCI enumeration, device drivers

Start here: **[docs/README.md](docs/README.md)**

## Contributing

Contributions are welcome. This project values clarity and simplicity:

- Keep implementations small and readable
- Prefer clear comments that explain the "why"
- Favor portable approaches and conservative memory use
- Update docs alongside code changes

See `CONTRIBUTING.md` for guidelines.

## License

EYN-OS is dedicated to the public domain. See `UNLICENSE` for details.
