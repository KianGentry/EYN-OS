# EYN-OS Development Roadmap & TODO

**Last Updated:** September 17, 2025  
**Version:** Release 14  
**Status:** Active Development  

This document outlines the comprehensive development plan for modernizing EYN-OS with essential features while maintaining the "built from the ground up" philosophy.

## Priority Features (Essential)

### 1. **Multi-Shell System** 
**Priority:** HIGH | **Status:** PLANNED | **Complexity:** MEDIUM

#### 1.1 Shell Architecture Redesign
- [ ] **Shell Registry System**
  - Create `src/utilities/shells/` directory structure
  - Implement `shell_interface_t` abstraction layer
  - Build shell plugin registration system
  - Add runtime shell switching capability

- [ ] **Default Shell Enhancement**
  - Refactor current shell into `src/utilities/shells/eyn_shell.c`
  - Maintain backward compatibility with existing commands
  - Enhance command history with search functionality
  - Add shell configuration persistence in EYNFS

- [ ] **Bash-Compatible Shell**
  - Implement `src/utilities/shells/bash_shell.c`
  - Add advanced scripting features (variables, conditionals, loops)
  - Support for command substitution `$(command)`
  - Implement globbing patterns (`*.txt`, `dir/*`)
  - Add environment variable support

- [ ] **Fish-Like Shell**
  - Create `src/utilities/shells/fish_shell.c`
  - Implement auto-suggestions and completions
  - Add syntax highlighting for commands
  - Smart history search with fuzzy matching

- [ ] **Shell Management Commands**
  - `shell list` - Show available shells
  - `shell switch <name>` - Change active shell
  - `shell config` - Configure shell preferences
  - `shell install <shell_file>` - Install new shell plugins

**Files to Create:**
```
include/shell_interface.h
src/utilities/shells/shell_registry.c
src/utilities/shells/eyn_shell.c
src/utilities/shells/bash_shell.c
src/utilities/shells/fish_shell.c
```

---

### 2. **USB Support System**
**Priority:** HIGH | **Status:** PLANNED | **Complexity:** HIGH

#### 2.1 USB Host Controller Drivers
- [ ] **UHCI Driver** (`src/drivers/usb/uhci.c`)
  - Universal Host Controller Interface for older systems
  - Interrupt-driven I/O handling
  - Root hub management
  - Transaction scheduling

- [ ] **OHCI Driver** (`src/drivers/usb/ohci.c`)
  - Open Host Controller Interface
  - Endpoint descriptor management
  - Transfer descriptor handling
  - Host controller initialization

- [ ] **EHCI Driver** (`src/drivers/usb/ehci.c`)
  - Enhanced Host Controller Interface for USB 2.0
  - High-speed transaction support
  - Periodic and asynchronous scheduling
  - Companion controller support

#### 2.2 USB Core Stack
- [ ] **USB Core** (`src/drivers/usb/usb_core.c`)
  - Device enumeration and configuration
  - Pipe management and data transfer
  - USB descriptor parsing
  - Hot-plug detection and handling

- [ ] **USB HID Driver** (`src/drivers/usb/usb_hid.c`)
  # EYN-OS Roadmap (concise)

  Last updated: 2025-09-17

  This file contains the essential development checklist and the planned implementation order for modernizing EYN-OS. It is intentionally concise and focused on action items.

  Checklist (essentials)
  - [ ] Multi-shell system (shell registry, runtime switching)
  - [ ] Paging / virtual memory (enable paging, page-fault handler)
  - [ ] Preemptive multitasking (context switch, scheduler, IPC basics)
  - [ ] USB support (host controllers, HID, mass storage)
  - [ ] Graphics (VBE framebuffer, basic 2D, windowing foundation)
  - [ ] Filesystem improvements (journaling, basic ACLs/symlinks)
  - [ ] Advanced I/O & hardware (AHCI/NVMe, network, audio, ACPI)
  - [ ] Development tools (editor improvements, debugger, assembler)
  - [ ] Resource monitor and basic profiling tools

  Planned implementation order

  Phase 1 — Foundation
  - Enable and validate paging and virtual memory
  - Implement minimal preemptive scheduler and context switch
  - Introduce shell registry and refactor current shell into a pluggable module
  - Add a basic resource monitor (`top`/`ps`)

  Phase 2 — Core systems
  - Implement USB core and at least one host controller driver (UHCI or EHCI)
  - Complete multitasking (process lifecycle, simple IPC: pipes)
  - Add VBE framebuffer support and basic 2D drawing primitives
  - Improve filesystem safety with journaling support

  Phase 3 — Platform features
  - Implement storage drivers (AHCI / NVMe) and integrate with filesystem
  - Add network and audio drivers
  - Implement ACPI basics (power and suspend/resume)

  Phase 4 — Developer UX and polish
  - Improve built-in editor and assembler/debugger tooling
  - Add profiling and monitoring utilities, expand resource monitor
  - Stabilize, optimize, and document

  Next immediate actions
  - Enable paging in `src/entry/kernel.c` and run a minimal validation boot
  - Add `include/shell_interface.h` and a `src/utilities/shells/` registry stub
  - Add a minimal `src/utilities/monitor/resource_monitor.c` that exposes CPU/memory stats

  Keep this file minimal; implementation notes and detailed design should live in per-feature design documents under `docs/`.

  - CPU benchmark tests
  - Memory bandwidth tests
  - Storage performance tests
  - Graphics performance tests

**Files to Create:**
```
src/utilities/monitor/resource_monitor.c
src/utilities/monitor/performance_tools.c
src/utilities/system/hardware_info.c
src/utilities/system/system_logs.c
src/utilities/profiling/cpu_profiler.c
src/utilities/profiling/memory_profiler.c
src/utilities/benchmark/benchmark_suite.c
```

---

## 🔧 Low-Level Infrastructure Improvements

### Memory Management Enhancements
- [ ] **Page Frame Allocator**
  - Buddy system for efficient memory allocation
  - Memory fragmentation reduction
  - NUMA-aware allocation (future multicore support)

- [ ] **Virtual Memory Subsystem**
  - Demand paging with page replacement
  - Memory-mapped files
  - Copy-on-write optimization
  - Swap file support

### Interrupt and Exception Handling
- [ ] **Advanced IRQ Management**
  - IRQ sharing and chaining
  - IRQ affinity for multicore systems
  - High-resolution timers
  - Deferred interrupt handling

### Boot System Improvements
- [ ] **Enhanced Boot Loader**
  - Multi-boot configuration
  - Boot menu with options
  - Hardware detection during boot
  - Recovery mode support

---

## Implementation Phases

### **Phase 1: Foundation (Weeks 1-4)**
1. Complete paging implementation and enable it
2. Enhance current scheduler with preemptive capabilities
3. Implement basic multi-shell system
4. Create resource monitoring framework

### **Phase 2: Core Systems (Weeks 5-8)**
1. Implement USB host controller drivers
2. Complete advanced multitasking with IPC
3. Begin graphics system with VESA support
4. Enhance filesystem with journaling

### **Phase 3: Advanced Features (Weeks 9-12)**
1. Complete graphics system with GUI framework
2. Implement advanced I/O drivers (AHCI, audio)
3. Build comprehensive development tools
4. Add performance monitoring and profiling

### **Phase 4: Polish and Integration (Weeks 13-16)**
1. System testing and debugging
2. Performance optimization
3. Documentation updates
4. Feature integration testing

---

## Build System Updates

### Makefile Enhancements Needed
```makefile
# Add new object files for each feature
USB_OBJS = obj/usb_core.o obj/uhci.o obj/ohci.o obj/ehci.o obj/usb_hid.o obj/usb_storage.o
GRAPHICS_OBJS = obj/vbe.o obj/framebuffer.o obj/window_manager.o obj/gui_framework.o
MULTITASK_OBJS = obj/scheduler.o obj/process.o obj/ipc.o obj/context_switch.o
SHELL_OBJS = obj/shell_registry.o obj/eyn_shell.o obj/bash_shell.o obj/fish_shell.o
FS_OBJS = obj/eynfs_v2.o obj/journal.o obj/compression.o obj/permissions.o
DEV_OBJS = obj/advanced_editor.o obj/eyn_debugger.o obj/enhanced_assembler.o
MONITOR_OBJS = obj/resource_monitor.o obj/performance_tools.o obj/system_logs.o

# Update main OBJS line
OBJS = $(CORE_OBJS) $(USB_OBJS) $(GRAPHICS_OBJS) $(MULTITASK_OBJS) $(SHELL_OBJS) $(FS_OBJS) $(DEV_OBJS) $(MONITOR_OBJS)
```

---

## Success Metrics

### Performance Targets
- Boot time: < 5 seconds
- Shell responsiveness: < 100ms command response
- Graphics: 60 FPS for simple animations
- Memory efficiency: < 8MB baseline usage
- USB detection: < 2 seconds for device enumeration

### Feature Completeness
- [ ] All 8 major feature areas implemented
- [ ] Comprehensive test suite passing
- [ ] Documentation updated for all new features
- [ ] Backward compatibility maintained
- [ ] System stability under load testing

---

## Next Immediate Actions

1. **Enable Paging System**: Uncomment paging initialization in `kernel.c` and fix any issues
2. **Shell Registry**: Create the shell interface abstraction layer
3. **USB Core**: Begin with USB core infrastructure and UHCI driver
4. **Resource Monitor**: Implement basic `top` command for system monitoring
