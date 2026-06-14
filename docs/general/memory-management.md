# Memory Management with Paging - EYN-OS

EYN-OS now includes a comprehensive virtual memory management system with paging support, providing memory protection, process isolation, and efficient memory allocation. **Note: This system is currently in development and not all features are fully functional yet.**

## Overview

The paging system implements:
- **Virtual Memory**: 4GB virtual address space with 4KB pages
- **Memory Protection**: User/supervisor privilege levels and read/write permissions
- **Process Isolation**: Separate page directories for each process
- **Demand Paging**: Pages allocated on-demand when accessed
- **Frame Management**: Efficient physical frame allocation and tracking

Low-RAM notes:
- Boot-time RAM pressure is heavily influenced by the kernel's **in-memory** footprint (not ISO size).
- Large zero-initialized globals in **`.bss` consume RAM at boot**; prefer on-demand allocations for infrequently-used large buffers.

## Architecture

### Memory Layout

```
Virtual Address Space (4GB):
0x00000000 - 0x3FFFFFFF  User Space (1GB)
0x40000000 - 0x7FFFFFFF  User Code (1GB)
0x80000000 - 0xBFFFFFFF  User Heap (1GB)
0xC0000000 - 0xFFFFFFFF  Kernel Space (1GB)

Physical Memory:
0x00000000 - 0x000FFFFF  Real Mode & BIOS
0x00100000 - 0x001FFFFF  Kernel Code & Data
0x00200000 - 0x007FFFFF  Available Memory (Heap)
0x00800000 - 0x00FFFFFF  High Memory (if available)
```

### Page Directory Structure

Each process has its own page directory containing:
- **Page Tables**: 1024 page tables per directory
- **Physical Addresses**: Direct mapping to physical frames
- **Permissions**: Present, Read/Write, User/Supervisor flags

### Frame Management

- **Frame Bitmap**: Tracks used/free 4KB physical frames
- **Best-Fit Allocation**: Efficient frame allocation strategy
- **Automatic Cleanup**: Frames freed when processes terminate

## Key Features

### Virtual Memory

```c
// Map a virtual page to a physical frame
void map_page(uint32 virtual_addr, uint32 physical_addr, int user, int rw);

// Unmap a virtual page
void unmap_page(uint32 virtual_addr);

// Get page information
page_t* get_page(uint32 address, int make, page_directory_t* dir);
```

### Memory Protection

```c
// Protect a page (make read-only)
void protect_page(uint32 virtual_addr);

// Unprotect a page (make read-write)
void unprotect_page(uint32 virtual_addr);

// Check page permissions
int is_page_present(uint32 virtual_addr);
int is_page_user(uint32 virtual_addr);
int is_page_writable(uint32 virtual_addr);
```

### Process Memory Management

```c
// Create a new page directory for a process
page_directory_t* create_process_page_directory(void);

// Switch to a process's page directory
void switch_to_process(page_directory_t* dir);

// Clean up process memory
void destroy_process_page_directory(page_directory_t* dir);
```

### Kernel Memory Allocation

```c
// Allocate aligned kernel memory
void* kmalloc_a(uint32 size);

// Allocate kernel memory with physical address
void* kmalloc_p(uint32 size, uint32* physical);

// Allocate aligned kernel memory with physical address
void* kmalloc_ap(uint32 size, uint32* physical);

// Free kernel memory
void kfree(void* ptr);
```

## Rust Allocator Integration

EYN-OS supports an incremental Rust migration path for allocator and mapped-buffer safety logic.

### Build Toggle

- `CONFIG_RUST_ALLOCATOR=0` (default): allocator logic uses C-only paths.
- `CONFIG_RUST_ALLOCATOR=1`: allocator and zero-copy paths call Rust FFI helpers for checked arithmetic and validation.

Important behavior:

- The C allocator now has compile-time fallback logic for all Rust helper call sites.
- This prevents unresolved-symbol link failures in non-Rust builds.

### C/Rust FFI Boundary

Rust helper declarations live in:

- `include/utilities/rust_alloc.h`

Rust implementations live in:

- `src/rust/rust_alloc.rs`

Current Rust-owned responsibilities:

- Zero-copy buffer create/destroy/read/write bounds safety.
- Heap block metadata validation checks.
- Allocation size/alignment math.
- Split/coalesce planning math with overflow checks.
- Pointer-to-block-offset conversion checks for free/realloc.
- Best-fit candidate update decision.
- Realloc copy-size/reallocate decision math.

Current C-owned responsibilities:

- Heap metadata mutation (`block->next`, `block->used`, final writebacks).
- Capability/context policy checks.
- User-visible diagnostics/log formatting.

### Testing the Rust Allocator Path

Run the dedicated smoke target:

```bash
make test-rust-allocator
```

This validates:

- util build with Rust path OFF.
- Rust helper object build.
- util/zero-copy build with Rust path ON.
- Full kernel link with Rust path ON.

### Troubleshooting Undefined Rust Symbols

If you see linker errors like undefined reference to `rust_*` symbols:

1. Ensure objects were built consistently with the same `CONFIG_RUST_ALLOCATOR` value.
2. Rebuild with explicit mode (or clean rebuild):

```bash
make clean
make all ARCH=i386 CONFIG_RUST_ALLOCATOR=1
```

3. Verify Rust target availability:

```bash
rustup target add i686-unknown-linux-gnu
```

## Page Fault Handling

The system includes intelligent page fault handling:

EYN-OS-specific notes:
- Many user-mode not-present faults are expected under demand paging/swap and can be recoverable.
- For performance (especially under low RAM), recoverable user-mode faults may be handled without printing verbose diagnostics.

### Page Fault Types
- **Not Present**: Page not mapped in virtual memory
- **Read-Only**: Attempted write to read-only page
- **User Access**: User process accessing kernel page
- **Reserved Bit**: Invalid page table entry

### Automatic Recovery
- **Demand Paging**: Automatically allocates pages on first access
- **Copy-on-Write**: Efficient memory sharing between processes
- **Stack Growth**: Automatic stack expansion

## Usage Examples

### Basic Memory Allocation

```c
// Allocate 1KB of kernel memory
void* ptr = kmalloc_a(1024);
if (ptr) {
    // Use memory
    memset(ptr, 0, 1024);
    kfree(ptr);
}
```

### Process Memory Management

```c
// Create a new process page directory
page_directory_t* proc_dir = create_process_page_directory();

// Map user code space
map_page(0x40000000, physical_addr, 1, 1); // User, RW

// Switch to process
switch_to_process(proc_dir);

// Clean up when process terminates
destroy_process_page_directory(proc_dir);
```

### Memory Protection

```c
// Make a page read-only
protect_page(0x40000000);

// Later, make it writable again
unprotect_page(0x40000000);
```

## Performance Characteristics

- **Page Size**: 4KB (optimal for most workloads)
- **TLB Efficiency**: High cache hit rates for repeated access
- **Memory Overhead**: ~4MB for page tables (1024 processes)
- **Allocation Speed**: O(1) for frame allocation
- **Protection Overhead**: Minimal performance impact

## Integration with Existing Systems

### Heap Management
The paging system works alongside the existing heap manager:
- Kernel heap uses `kmalloc_*` functions
- User heap allocated through page mapping
- Automatic cleanup on process termination

### Process Isolation
Enhanced process isolation:
- Separate virtual address spaces
- Memory protection between processes
- Automatic cleanup on process exit

### File System
Improved file system integration:
- Memory-mapped files (future feature)
- Efficient buffer management
- Protected file access

## Future Enhancements

### Planned Features
- **Memory-Mapped Files**: Map files directly into memory
- **Shared Memory**: Inter-process memory sharing
- **Huge Pages**: 2MB and 1GB page support
- **Memory Compression**: Compress rarely-used pages
- **NUMA Support**: Non-uniform memory access

### Performance Optimizations
- **Page Prefetching**: Anticipate page access patterns
- **Memory Defragmentation**: Compact physical memory
- **Smart Caching**: Adaptive cache management
- **Memory Pooling**: Specialized allocators for common sizes

## Troubleshooting

### Common Issues

**Page Faults**
- Check if address is valid
- Verify page permissions
- Ensure page is mapped

**Memory Exhaustion**
- Monitor frame bitmap usage
- Check for memory leaks
- Consider reducing process count

**Performance Issues**
- Monitor TLB miss rates
- Check page table fragmentation
- Optimize memory access patterns

### Debugging Commands

```bash
# Show memory statistics
memory stats

# Check page protection status
memory protect

# Monitor page faults
error details

# Intentionally trigger a fault (for testing)
pf yes 0x0 r
```

## Conclusion

The paging system provides EYN-OS with modern memory management capabilities while maintaining the system's educational focus and performance characteristics. It enables true process isolation, efficient memory usage, and provides a foundation for future advanced features.
