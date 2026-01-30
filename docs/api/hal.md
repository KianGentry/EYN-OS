# HAL (Hardware Abstraction Layer)

The HAL is the contract boundary that makes EYN-OS a **port** rather than a fork.

## Principle

Shared subsystems (shell/TUI, VFS, netstack, userland ABI, syscall layer) must not call architecture- or platform-specific functions directly.

Instead, they call HAL interfaces declared under `include/hal/`, whose implementations are provided by:

- i386 backends (PIC/PIT, PS/2, VGA/linear framebuffer, ATA/IDE, PCI NIC)
- AArch64 backends (GIC/generic timer, virtio-input, ramfb, virtio-blk, virtio-net)

The only allowed differences between targets should be the build flags and which backend objects are linked.

## Header overview

- `include/hal/console.h`: kernel console output (text stream)
- `include/hal/keyboard.h`: non-blocking character input + modifier state
- `include/hal/time.h`: ticks/timebase + cooperative sleep
- `include/hal/block.h`: sector I/O for block devices (read/write/flush)
- `include/hal/irq.h`: register/enable/disable IRQ handlers (logical IRQ IDs)
- `include/hal/mm.h`: page allocation and map/unmap primitives (kernel)
- `include/hal/net.h`: network device access (wraps `include/network/netdev.h`)
- `include/hal/hal.h`: convenience umbrella include

## Contracts (important)

### General

- All HAL APIs must be safe in a freestanding environment (no libc assumptions).
- If a function is callable from IRQ context, it must say so.
- Return values must be stable and consistent across architectures.

### Console

- `hal_console_putc()` and `hal_console_write()` must be safe to call from early boot.
- Control characters must match the shell/TUI expectations:
  - `\n` new line
  - `\r` carriage return
  - `\b` and `0x7F` (DEL) act as backspace/delete for line editing

### Keyboard

- `hal_kbd_getc_nonblock()` returns 0 when no input is available.
- Returned values are bytes of the unified input stream used by shell/TUI/userland (ASCII plus control bytes like `\b`, `\n`, `0x1B`).

### Block

- Sector size is assumed to be 512 bytes for now.
- Reads/writes must be coherent with DMA requirements on the platform (cache maintenance on AArch64 backends).

### IRQ

- Logical IRQ IDs are backend-defined but must be stable per platform.
- Shared code should not assume PIC-style 0-15 numbering.

## Layering rules

- Shared code should include `hal/hal.h` (or a specific `hal/*.h`) and avoid including arch-specific headers.
- Backend implementations may include hardware headers (`cpu/aarch64/*`, `drivers/aarch64/*`, etc.) as needed.

## Migration

This HAL starts as a set of headers (contracts). As parity work progresses we:

1. Add thin wrappers so both i386 and AArch64 provide each HAL symbol.
2. Switch shared call sites to the HAL incrementally.
3. Add transcript tests to ensure behavior stays identical.
