# Cross-Architecture Parity (i386 <-> AArch64)

This document defines what "identical experience" means for EYN-OS across build targets.

## Goal

A feature developed for i386 should be usable on AArch64 with **no code changes above the HAL boundary**. The only differences should be:

- Toolchain and linker script
- Platform/board bring-up (device discovery, MMU/interrupt controller init)
- Device driver backends (PS/2 vs virtio-input, IDE vs virtio-blk, PCI NIC vs virtio-net)

Everything else (shell/TUI, VFS behavior, userland ABI, syscalls, networking stack, UI/tiler APIs) should remain shared.

## Non-goals

- Identical performance characteristics
- Identical physical device support (real hardware backends differ)
- Exact same boot transport (Multiboot vs DTB) beyond exposing the same higher-level services

## How parity is measured

Parity is not subjective: we treat it as a set of observable behaviors.

### Smoke transcript (minimum bar)
A single scripted session should succeed on both targets:

- Boot to shell prompt
- `help` shows the same command set and help text
- `ls /`, `cat /test.txt`, `vfsdetect` produce equivalent output semantics
- Basic line editing works (backspace/delete, arrows, ctrl shortcuts)

### Feature-level checks
For each subsystem below, we track:

- **User-visible behavior** (what the user sees/does)
- **API surface** (headers used by shared code)
- **Backends** (per-arch implementations)
- **Regression test** (a command or transcript)

## Parity status matrix

Legend:
- ✅ implemented and validated
- 🟡 implemented but incomplete / needs parity polish
- ❌ missing
- ⬜ not assessed

| Subsystem | i386 | AArch64 | Notes / parity definition | Primary test |
|---|---:|---:|---|---|
| Boot to interactive shell | ✅ | 🟡 | AArch64 currently has bring-up shell + a "full" build path; final target is same shell/TUI entrypoint on both. | smoke transcript |
| Console semantics | ✅ | 🟡 | Control chars, scrolling, cursor, colors. AArch64 output now routes through the HAL console, but ramfb still needs parity polish vs VGA. | `help` + edit line |
| Keyboard semantics | ✅ | 🟡 | Same keybindings, modifiers, arrows/home/end, ctrl shortcuts, repeat. Backends differ but output stream must match. | edit line + ctrl shortcuts |
| Mouse / pointer | ✅ | ⬜ | Same UI interactions in tiler. | UI smoke |
| Timer / ticks | ✅ | 🟡 | Same tick rate expectations, sleep accuracy, watchdog behavior. | `ticks` + timer selftest |
| Interrupt masking rules | ✅ | 🟡 | Shared code can assume the same IRQ save/restore semantics. | stress / watchdog |
| Block device access | ✅ | ✅ | AArch64 uses virtio-blk via ATA compatibility bridge; future is HAL block API. | VFS read/write |
| VFS read semantics | ✅ | ✅ | `vfs_stat`, `vfs_listdir`, `vfs_read_file` behavior must match. | `ls`, `cat` |
| VFS write semantics | ✅ | 🟡 | Confirm write paths behave the same on both (truncate, mkdir/rmdir, unlink). | write/mkdir tests |
| Userland loading (.uelf) | ✅ | ⬜ | Same loader behavior, argv/env, fd routing, exit codes. | run hello/demo |
| Syscall ABI | ✅ | ⬜ | Same syscall numbers/struct layouts/error returns. Only entry/return is arch-specific. | userland tests |
| Scheduler behavior | ✅ | ⬜ | Same yield/sleep semantics from userland and shell. | sleep/yield tests |
| Networking stack | ✅ | ⬜ | Shared stack; only netdev backend differs. | net commands |
| GUI / tiler APIs | ✅ | ⬜ | Same tile_manager API; same UI output. | GUI demo |

## Rules to prevent drift

1. Shared subsystems may only depend on headers under:
   - `include/` that are not architecture-specific, and
   - the HAL interfaces in `include/hal/`
2. Architecture-specific code must live under an arch folder (e.g. `src/cpu/aarch64/`, `src/drivers/aarch64/`) and must not leak symbols into shared code except via HAL.
3. Any new user-facing feature must have:
   - a parity row update in this document, and
   - a simple test/transcript entry.

## Where the HAL lives

See: `docs/api/hal.md`
