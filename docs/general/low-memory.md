# Low-Memory Notes (4–9MB QEMU targets)

EYN-OS is intentionally usable in extremely low RAM configurations (the default QEMU run config is very small). This document summarizes the recent memory/boot improvements and the resulting runtime behavior.

## Boot-time: kernel footprint matters (especially `.bss`)

When booting via GRUB/Multiboot, the kernel's *in-memory* footprint (not just the ISO size) must fit.

Key point: **Large zero-initialized globals live in `.bss` and still consume RAM at boot**, because the loader must reserve and zero that space.

Recent improvement:
- Large write-editor buffers were moved from static `.bss` storage to **on-demand heap allocations** that are only made when the editor is actually opened.

Practical guidance:
- Avoid multi-hundred-KB `static`/global arrays in kernel code.
- Prefer *lazy allocation* (allocate on first use) and *prompt cleanup* (free on exit) for infrequently-used subsystems.

Host-side ways to spot footprint regressions:
```bash
# After a build, inspect section sizes
i686-elf-size -A tmp_user/boot/kernel.bin || size -A tmp_user/boot/kernel.bin

# Find large symbols (good for hunting accidental big .bss)
i686-elf-nm -S --size-sort tmp_user/boot/kernel.bin | tail -n 50
```

## Runtime: "page faults" can be normal under demand paging

EYN-OS uses demand paging and swap. That means a #PF (INT 14) is not automatically a crash:

- **Expected (recoverable)**: user-mode access to a *non-present* page that is marked as demand-zero or swapped-out.
- **Unexpected (fatal)**: kernel-mode faults, reserved-bit violations, or user-mode protection faults that cannot be resolved.

Recent improvement:
- **Handled user-mode page faults no longer spam verbose dumps** by default.
  - Why: in low RAM, demand paging/swap can fault frequently; printing (serial/VGA) on every handled fault can dominate runtime and make the UI appear choppy.

## User programs: large `.bss` is now low-RAM-friendly

The user program loader sets up the program's full `PT_LOAD` mapping range as **demand-zero** first, then only allocates/maps the pages that actually contain file-backed bytes.

Result:
- Large user `.bss` regions (and similar zero-fill spans) do **not** immediately allocate physical frames.
- Physical frames are allocated on first access, via the VMM fault handler.

This makes programs with large static buffers much more likely to start successfully under very low RAM.

## Debugging in low RAM

- If you hit a real crash, use the panic/stop-code docs first: `docs/stop-codes.md`.
- For paging issues, capture serial logs: `make qemu-debug`.
- Useful runtime commands:
  - `memory stats` (frame/heap overview)
  - `error details` (recent error context)
  - `pf yes 0x0 r` (intentionally trigger a fault for testing)
