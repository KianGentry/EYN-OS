# EYN-OS Documentation

Welcome to the EYN-OS documentation! This directory contains comprehensive documentation for all aspects of the EYN-OS operating system.

## Documentation Structure

### Core System
- **[System Overview](system-overview.md)** - High-level architecture and design principles
- **[Boot Process](boot-process.md)** - How EYN-OS boots from GRUB to kernel
- **[Memory Management](memory-management.md)** - Memory allocation and management
- **[Memory Management with Paging](memory-management.md)** - Virtual memory and paging system
- **[Interrupts & Exceptions](interrupts.md)** - IDT, ISRs, and exception handling
- **[Panic & Assertions](general/panic.md)** - Panic screen, assertions, and developer macros
- **[Watchdog Timer](general/watchdog.md)** - Detecting stalls and tuning timeouts

### Filesystems
- **[EYNFS](filesystems/eynfs.md)** - EYN-OS native filesystem specification
- **[FAT32 Support](filesystems/fat32.md)** - FAT32 filesystem driver
- **[Filesystem Commands](filesystems/commands.md)** - `ls`, `cd`, `del`, etc.
- **[REI Image Format](filesystems/rei-format.md)** - Custom image format specification

### User Interface
- **[TUI System](ui/tui.md)** - Text User Interface framework
- **[Shell System](ui/shell.md)** - Command-line interface and shell commands
- **[Tiling Manager](ui/tiling-manager.md)** - Tile-based UI and floating windows (GUI API)
- **[Stats Monitor](ui/stats-gui.md)** - Graphical CPU/memory/disk monitor
- **[Pipeline System](pipeline-system.md)** - Unix-like pipelines and redirection
- **[Help System](ui/help.md)** - Interactive help command

### Development Tools
- **[Assembler](tools/assembler.md)** - Built-in NASM-compatible assembler
- **[Executable Format](tools/eyn-format.md)** - EYN executable format specification
- **[Program Loading](tools/loader.md)** - How user programs are loaded and executed

### Games & Applications
- **[Game Engine](applications/game-engine.md)** - Built-in game framework
- **[Snake Game](applications/snake.md)** - Snake game implementation
- **[Write Editor](applications/write-editor.md)** - Text editor documentation

### Utilities
- **[Math Library](utilities/math.md)** - Random number generation, sorting, search algorithms
- **[String Functions](utilities/string.md)** - String manipulation utilities
- **[System Utilities](utilities/system.md)** - System-level utility functions

### API Reference
- **[Header Files](api/headers.md)** - Complete header file documentation
- **[Function Reference](api/functions.md)** - All public API functions
- **[Data Structures](api/structures.md)** - Important data structures and types
- **[Syscalls](api/syscalls.md)** - System call interface and programming guide

### Troubleshooting
- **[Stop Codes Guide](stop-codes.md)** - Interpret panic stop codes (EYNOS_XXXXXXXX) and next steps

## Quick Start

1. **For Users**: Start with [System Overview](system-overview.md) and [Shell System](ui/shell.md)
2. **For Developers**: Begin with [System Overview](system-overview.md) and [API Reference](api/headers.md)
3. **For Filesystem Work**: See [EYNFS](filesystems/eynfs.md) and [Filesystem Commands](filesystems/commands.md)

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
- `docs/whats-new-dev.md` - Summary of recent changes on dev
