# EYN-OS System Overview

EYN-OS is a freestanding x86 operating system designed with the philosophy of "reinventing the wheel" - building everything from scratch to understand and control every aspect of the system (within reason).

## Architecture Overview

### Target Platform
- **Architecture**: Intel x86 (32-bit)
- **Bootloader**: GRUB (Multiboot 1.0 compliant, ultra-minimal 6.3MB ISO)
- **Memory Model**: Flat memory model (no segmentation/paging)
- **Target Hardware**: Low-end systems (3MB RAM minimum with GRUB, 1MB with direct boot, optimized for 128KB+ systems)
- **ISO Size**: 6.3MB (78% reduction from original 29MB)

### Core Design Principles
1. **Freestanding Environment**: No dependency on standard C libraries
2. **From-Scratch Implementation**: Custom implementations of all system components
3. **Educational Focus**: Clear, understandable code for learning purposes
4. **Modular Design**: Well-separated components for maintainability
5. **Stability & Security**: Robust error handling and graceful failure recovery
6. **Extreme Portability**: Optimized for systems with minimal RAM

## System Components

### Kernel Core
- **Boot Process**: GRUB → Kernel entry point → System initialization
- **Memory Management**: Advanced heap management with corruption detection
- **Interrupt Handling**: Intelligent exception handling with recovery mechanisms
- **Device Drivers**: VGA, keyboard, ATA disk controller

### Stability & Security Features
- **Exception Recovery**: Intelligent ISR handlers that attempt recovery instead of halting
- **Memory Protection**: Heap corruption detection, stack overflow protection
- **Command Safety**: Input validation, argument sanitization, injection prevention
- **Process Isolation**: Memory separation between kernel and user programs
- **Error Logging**: Comprehensive error tracking and reporting system

### Memory Management
- **Dynamic Heap Sizing**: Adaptive memory allocation based on available RAM
- **Corruption Detection**: Magic numbers, checksums, and block validation
- **Best-Fit Allocation**: Efficient memory allocation strategy
- **Memory Leak Detection**: Tracking of allocation and free operations
- **Conservative Limits**: Prevents excessive memory usage on low-end systems

### Filesystem Layer
- **EYNFS**: Native filesystem with custom design
- **FAT32 Support**: Compatibility with existing filesystems
- **File Operations**: Read, write, create, delete, directory traversal
- **Dynamic Buffering**: Adaptive file reading with up to 64KB buffers

### User Interface
- **TUI Framework**: Text-based user interface system
- **Shell System**: Command-line interface with streaming command architecture
- **Help System**: Interactive documentation viewer with dual-pane layout
- **File Rendering**: Support for REI images and Markdown formatting

### Development Tools
- **Assembler**: Built-in NASM-compatible assembler
- **Executable Format**: Custom EYN format for user programs
- **Program Loader**: Safe execution of user programs with process isolation

### Applications
- **Game Engine**: Framework for built-in games (Snake, Maze Runner)
- **Text Editor**: Write editor for file editing
- **Utilities**: Math functions, search, sort, random number generation
- **File Operations**: Copy, move, and advanced file management

## File Structure

```
EYN-OS/
├── src/
│   ├── boot/           # Bootloader and kernel entry
│   ├── cpu/            # CPU management (IDT, ISRs)
│   ├── drivers/        # Hardware drivers
│   ├── entry/          # Kernel main entry point
│   └── utilities/      # System utilities and applications
├── include/            # Header files
├── docs/              # Documentation (this directory)
├── testdir/           # Test files and game data
└── Makefile           # Build system
```

## Build System

### Compilation
- **Compiler**: GCC with freestanding flags
- **Linker**: Custom linker script for proper memory layout
- **Target**: ELF32 binary for GRUB compatibility

### Key Build Flags
```bash
-m32                    # 32-bit x86 target
-ffreestanding         # No standard library
-fno-stack-protector   # Disable stack protection
-Oz                    # Optimize for size
```

## Boot Process

### GRUB Boot (Standard)
1. **GRUB Loads**: Multiboot header recognized by GRUB
2. **Kernel Entry**: `kernel.asm` sets up initial stack and calls `kernel.c`
3. **Memory Detection**: Dynamic detection of available RAM using multiboot info
4. **System Init**: Initialize IDT, drivers, filesystem with adaptive sizing
5. **Shell Launch**: Start command-line interface with streaming commands
6. **User Interaction**: Ready for user commands

### Direct Boot (Ultra-Low Memory)
1. **Direct Load**: Kernel loaded directly by QEMU/emulator
2. **Kernel Entry**: `kernel.asm` sets up initial stack and calls `kernel.c`
3. **Memory Detection**: Dynamic detection of available RAM using multiboot info
4. **System Init**: Initialize IDT, drivers, filesystem with adaptive sizing
5. **Shell Launch**: Start command-line interface with streaming commands
6. **User Interaction**: Ready for user commands

### Boot Method Selection
- **Use GRUB**: For systems with 3MB+ RAM, provides boot menu and configuration
- **Use Direct Boot**: For systems with 1-2MB RAM, bypasses bootloader overhead
- **Memory Savings**: Direct boot saves ~2MB of RAM by eliminating GRUB requirements

## Memory Layout

```
0x00000000 - 0x000FFFFF  # Real mode and BIOS
0x00100000 - 0x001FFFFF  # Kernel code and data
0x00200000 - 0x007FFFFF  # Available memory (adaptive heap)
0x00800000 - 0x00FFFFFF  # High memory (if available)
```

## Hardware Support

### Supported Devices
- **VGA**: Text mode display (80x25 characters)
- **PS/2 Keyboard**: Basic keyboard input
- **ATA/IDE**: Hard disk and CD-ROM access
- **Serial**: Basic serial port communication

### Memory Requirements
- **Minimum**: 3MB RAM (with GRUB bootloader), 1MB RAM (with direct boot)
- **Recommended**: 8MB+ RAM (for comfortable usage with all features)
- **Optimal**: 16MB+ RAM (for full performance and multitasking)
- **Target**: Systems as low as 128KB RAM (future goal)
- **Note**: GRUB bootloader requires additional memory for its internal scripting system

## Streaming Command Architecture

### Essential Commands
Always available in RAM for core functionality:
- `init`, `ls`, `exit`, `clear`, `help`
- `memory`, `portable`, `load`, `unload`, `status`

### Streaming Commands
Loaded on-demand to conserve memory:
- Filesystem: `format`, `fdisk`, `fscheck`, `copy`, `move`, `del`
- Basic: `echo`, `ver`, `calc`, `search`, `drive`, `read`, `write`
- Advanced: `random`, `history`, `sort`, `game`, `draw`
- Subcommands: Various specialized command variants

## Development Philosophy

### Why "Reinvent the Wheel"?
1. **Learning**: Understanding how everything works
2. **Control**: Full control over system behavior
3. **Simplicity**: No unnecessary complexity
4. **Customization**: Tailored to specific needs

### Code Style
- **Clear Comments**: Extensive documentation in code
- **Simple Functions**: One function, one purpose
- **Consistent Naming**: Descriptive function and variable names
- **Error Handling**: Graceful error recovery where possible
- **Professional Output**: Clean, informative user messages

## Practical Usage

### Memory Requirements by Use Case
- **Basic CLI Usage**: 1MB RAM (direct boot)
- **File Operations**: 2MB RAM (direct boot)
- **Full Features**: 3MB RAM (GRUB boot)
- **Development Work**: 8MB+ RAM (comfortable)
- **Gaming**: 16MB+ RAM (optimal performance)

### Boot Method Selection Guide
- **1-2MB RAM Systems**: Use `make test_direct` for direct boot
- **3MB+ RAM Systems**: Use `make run` for GRUB boot
- **Testing/Development**: Use `make test` for 64MB configuration
- **Ultra-Low Memory**: Use `make test_direct` with `-m 1M` QEMU flag

### Memory Optimization Tips
- Use `unload` to free command memory when not needed
- Monitor memory usage with `memory stats`
- Use `portable stats` to see memory savings
- Consider direct boot for systems with very limited RAM

## Future Directions

### Planned Features
- **Protected Mode**: Full 32-bit protected mode with paging
- **Multitasking**: Basic process scheduling
- **Network Support**: TCP/IP stack
- **GUI System**: Graphical user interface
- **More Games**: Additional built-in games

### Extensibility
- **Module System**: Loadable kernel modules
- **Plugin Architecture**: Extensible application framework
- **API Stability**: Stable programming interfaces