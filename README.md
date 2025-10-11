# EYN-OS — a public domain x86 operating system

EYN-OS is a small, educational operating system for 32‑bit x86 built entirely from scratch. It aims to be clear, portable, and approachable, favoring simple designs that are easy to learn from and extend.

## What it provides

- A freestanding 32‑bit kernel with a straightforward boot path
- Basic drivers (display, keyboard, storage) and a simple userspace model
- A native filesystem alongside compatibility with common layouts
- A text‑first user interface with optional graphical elements
- Built‑in developer tools and sample applications
- Strong emphasis on robustness and portable, low‑memory operation

Rather than relying on external libraries, EYN-OS includes minimal, well‑documented implementations of core facilities such as memory management, interrupts/exceptions, basic device I/O, and simple graphics/text rendering.

## Architecture and philosophy

EYN-OS follows a “learnable core” approach:
- From‑scratch components with small, readable implementations (no libc)
- Clear layering: CPU/interrupts → drivers → kernel utilities → UI/tools
- Conservative memory footprint suitable for very low‑RAM targets
- Deterministic behavior prioritized over feature breadth

### What makes it different
- Education first: code explains the “why,” not just the “what”
- Minimal dependencies: predictable builds and easy portability
- Tight scope: only the primitives needed to understand an OS stack
- Practical docs: APIs and internals documented alongside the code

## Architecture at a glance

- Target: 32‑bit x86, Multiboot‑compatible boot (via GRUB)
- Memory: flat memory model with a compact heap and consistency checks
- Drivers: VGA‑style display, keyboard input, ATA/IDE storage, serial output
- Filesystems: native EYNFS plus simple interop paths for common formats
- UI: text‑based shell and TUI building blocks with a lightweight graphics layer

The codebase is organized by domain (CPU, drivers, misc, utilities) with public headers under `include/` and implementation in `src/`.

## Getting started

Build from a Linux host with standard toolchain components:

```bash
make run        # build and run in QEMU
```

To try it on real hardware, write `EYNOS.iso` to a USB drive and boot on a 32‑bit compatible machine.

## System requirements

- 32‑bit x86 CPU
- ~3 MB RAM or more (depending on boot method and use)
- VGA‑compatible display

## Documentation

The `docs/` directory contains the full project documentation, including architecture notes, UI and filesystem descriptions, and API references:

- System overview and design goals
- Boot, memory, interrupts/exceptions
- User interface and shell
- Filesystems and tools
- Public headers and APIs

Start here: `docs/README.md`.

## Contributing

Contributions are welcome. This project values clarity and simplicity:

- Keep implementations small and readable
- Prefer clear comments that explain the “why”
- Favor portable approaches and conservative memory use
- Update docs alongside code changes

See `CONTRIBUTING.md` for guidelines.

## License

EYN-OS is dedicated to the public domain. See `UNLICENSE` for details.
