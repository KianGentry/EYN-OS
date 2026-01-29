# AArch64 Bring-up Heap

The AArch64 bring-up and full-mode builds include a small freestanding heap allocator to enable incremental porting of existing kernel subsystems (shell utilities, parsers, drivers that allocate buffers, etc.).

## Goals

- Provide `malloc`/`free`/`realloc`/`calloc` in the AArch64 build without pulling in i386-only code.
- Stay safe on AArch64: alignment-aware allocations and no unaligned word accesses.
- Keep it small and predictable: this is a bring-up allocator, not the final VM-backed memory manager.

## Implementation

- Source: `src/utilities/aarch64/heap.c`
- API: `include/utilities/aarch64/heap.h`

The allocator:

- Places the heap immediately after the linker symbol `__kernel_end`.
- Caps the heap to the device-tree RAM region when available (`fdt_parse_memory`).
- Uses a simple singly-linked list of blocks and a first-fit search.
- Coalesces adjacent free blocks on `free` to reduce fragmentation.

## Initialization

Full-mode initializes the heap once RAM bounds are known:

- `src/entry/aarch64/kernel_full.c` calls `aarch64_heap_init(ram_base, ram_size)`.

If `aarch64_heap_init` is not called, `malloc` falls back to a small conservative heap window.

## Limitations

- Not thread-safe.
- No paging integration.
- Not intended as the final allocator for the full OS; it exists to unblock port work.
