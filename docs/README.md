# EYN-OS Documentation

Welcome to the EYN-OS documentation! This directory contains comprehensive documentation for all aspects of the EYN-OS operating system.

## Documentation Structure

### Core System
- **[System Overview](system-overview.md)** - High-level architecture and design principles
- **[Memory Management with Paging](mm/virtual-memory.md)** - Virtual memory and paging system
- **[Low-Memory Notes](general/low-memory.md)** - Boot/runtime behavior in 4–9MB RAM targets
- **[Panic & Assertions](general/panic.md)** - Panic screen, assertions, and developer macros
- **[Watchdog Timer](general/watchdog.md)** - Detecting stalls and tuning timeouts
- **[Pipeline System](general/pipeline-system.md)** - Unix-like pipelines and redirection

### Filesystems
- **[EYNFS](filesystems/eynfs.md)** - EYN-OS native filesystem specification
- **[FAT32 Support](filesystems/fat32.md)** - FAT32 filesystem driver
- **[MBR Partitioning](mm/virtual-memory.md#partitioning)** - Disk partitioning support
- **[REI Image Format](api/rei-api.md)** - Custom image format specification
- **[REIV Video Format](api/rei-api.md#reiv)** - Animation/video format (MP4/GIF conversion)

### User Interface
- **[TUI System](ui/tui.md)** - Text User Interface framework
- **[Shell System](ui/shell.md)** - Command-line interface and shell commands
- **[Shell Scripts](ui/shell-scripts.md)** - Shell scripting support
- **[Command Aliases](ui/shell.md#aliases)** - Persistent command aliases with parameters
- **[Tiling Manager](ui/tiling-manager.md)** - Tile-based UI and floating windows (GUI API)
- **[Theme Customization](ui/tiling-manager.md#themes)** - Customizable colours and fonts
- **[Stats Monitor](ui/stats-gui.md)** - Graphical CPU/memory/disk monitor
- **[Help System](ui/enhanced-help-system.md)** - Interactive help command
- **[Command Reference](command-reference.md)** - Complete command documentation (auto-generated)

### Development Tools
- **[Assembler](tools/assembler.md)** - Built-in NASM-compatible assembler
- **[C Compiler](tools/chibicc.md)** - Integrated chibicc C compiler (C11 compliant)
- **[X11 Compatibility](api/x11-compat.md)** - Source-level Xlib compatibility layer for GUI ports
- **[UELF Format](api/userland-uelf-abi.md)** - Userland ELF executable format specification
- **[Ring-3 Userspace](api/userland-uelf-abi.md#ring3)** - Privilege separation and protection
- **[Program Loading](tools/loader.md)** - How user programs are loaded and executed

### Games & Applications
- **[Game Engine](applications/game-engine.md)** - Built-in game framework
- **[DOOM Port](applications/doom.md)** - Native DOOM port and in-OS `build_doom` workflow
- **[Write Editor](ui/tui.md#write-editor)** - Text editor documentation
- **[GUI Applications](ui/tiling-manager.md#gui-clients)** - Creating GUI applications
- **[Second Reality](applications/second-reality.md)** - Second Reality demo port (visuals only)

### Hardware & Networking
- **[Networking Overview](network/README.md)** - UDP/IPv4 stack and e1000 driver
- **[E1000 Driver](network/e1000-driver.md)** - Intel NIC driver details
- **[Network Stack](network/network-stack.md)** - UDP, IPv4, ARP protocols
- **[PCI Enumeration](network/README.md#pci)** - Device detection and configuration
- **[Watchdog](general/watchdog.md)** - Hang detection and recovery

### Utilities
- **[Math Library](utilities/math.md)** - Random number generation, sorting, search algorithms
- **[String Functions](utilities/string.md)** - String manipulation utilities
- **[System Utilities](utilities/system.md)** - System-level utility functions

### API Reference
- **[Syscalls](api/syscalls.md)** - System call interface and programming guide (ring-3)
- **[UELF ABI](api/userland-uelf-abi.md)** - Userland executable format and conventions
- **[REI/REIV API](api/rei-api.md)** - Image and video format specifications
- **[Header Files](api/headers.md)** - Complete header file documentation

### Troubleshooting
- **[Quick Debug Reference](QUICK-DEBUG.md)** - One-page debugging cheat sheet (PRINT THIS!)
- **[Stop Codes Guide](stop-codes.md)** - Interpret panic stop codes (EYNOS_XXXXXXXX) and next steps
- **[Debugging Guide](general/debugging.md)** - Comprehensive debugging techniques and tools
- **[Component Reference](general/component-reference.md)** - Quick reference for each OS component

## Quick Start

1. **For Users**: Start with [System Overview](system-overview.md) and [Quick Reference](quick-reference.md)
2. **For Developers**: Begin with [System Overview](system-overview.md), [UELF ABI](api/userland-uelf-abi.md), and [Syscalls](api/syscalls.md)
3. **For Debugging**: See [Quick Debug Reference](QUICK-DEBUG.md) and [Stop Codes](stop-codes.md)
4. **For Networking**: See [Networking Overview](network/README.md) and [Quick Reference](quick-reference.md#networking-commands)
5. **For Filesystem Work**: See [EYNFS](filesystems/eynfs.md) and [Command Reference](command-reference.md#filesystem-commands)
6. **For Building**: Run `make docker-build` from the repository root; optionally run `make menuconfig` first if you need to adjust the final ISO configuration

## Contributing to Documentation

When adding new features to EYN-OS, please:
1. Update relevant documentation files
2. Add examples where appropriate
3. Include code snippets for complex concepts
4. Update this README if adding new sections

## Related Files

- `README.md` - Main project README
- `Makefile` - Build system documentation
- `src/` - Source code with inline documentation
- `include/` - Header files with function documentation
