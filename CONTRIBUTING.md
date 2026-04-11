# Contributing to EYN-OS

Thank you for helping improve EYN-OS. This project is a freestanding 32-bit x86 kernel and userland stack, so changes should prioritize correctness, clarity, and low-memory behavior.

## Before You Start

### Prerequisites
- GCC with 32-bit support (`gcc -m32`), or `i686-elf-gcc`
- NASM
- QEMU
- GRUB tooling (`grub2-mkrescue` or `grub-mkrescue`)
- Python 3

### Useful Entry Points
- Kernel entry: `src/entry/kernel.c`
- Linker script: `src/boot/link.ld`
- Interrupt/syscall path: `src/cpu/idt.c`, `src/cpu/isr.c`, `src/cpu/syscall.asm`
- Shell dispatcher: `src/utilities/shell/shell.c`

## Build, Run, Debug

- Build full artifacts (kernel + ramdisk + EYNFS image + docs + ISO):

```bash
make build
```

- Run in QEMU:

```bash
make run
```

- Run with serial and CPU/interrupt logging:

```bash
make qemu-debug
# logs: tmp/qemu-debug.log
```

- Start halted for GDB (`target remote :1234`):

```bash
make qemu-gdb
```

- Rebuild userland binaries from `EYN-packages/packages/*/*_uelf.c`:

```bash
make userland
```

- Rebuild filesystem image from `testdir/`:

```bash
make eynfsimg
```

- Static analysis pass:

```bash
make analyze
```

## Current Command Contribution Model

User-facing shell commands are now primarily userland binaries loaded from `/binaries`.

### Add a New User Command
1. Create source at `EYN-packages/packages/<name>/<name>_uelf.c`.
2. Add command metadata near the top:

```c
#include <eynos_cmdmeta.h>
EYN_CMDMETA_V1("Describe what this command does.", "<name> --example");
```

3. Build it to `testdir/binaries/<name>` using one of:

```bash
bash EYN-packages/devtools/build_user_c.sh packages/<name>/<name>_uelf.c ../testdir/binaries/<name>
```

4. Repack and run:

```bash
make eynfsimg
make run
```

5. Regenerate command docs/help text:

```bash
make docs
```

Notes:
- The shell resolves commands from `/binaries` first.
- Extensionless binaries are preferred (`/binaries/view`), but `.uelf` remains supported for compatibility.

## Coding Standards

### C and Build Conventions
- Use project types from `include/misc/types.h` (`uint8`, `uint32`, `string`, etc.) where appropriate.
- Use angle-bracket includes resolved by include paths, for example `#include <fs/vfs.h>`.
- Add function prototypes for new non-`static` functions in appropriate headers.
- Keep function names and variable names descriptive.
- Keep functions small and focused (target under ~100 lines when practical).
- Prefer small bounded buffers and streaming patterns over unbounded allocations.
- In hot paths (especially paging/interrupt handling), avoid noisy logging.

### Low-Memory Requirements
- Assume low-memory operation is a first-class target.
- Avoid large static/global arrays, especially in kernel `.bss`.
- Prefer lazy/on-demand allocation and free resources promptly.
- Be careful when introducing per-fault/per-iteration logs in frequently hit code.

### Debugging-Conscious Behavior
- Kernel-mode page faults are bugs and should fail fast.
- User-mode non-present faults may be expected under demand paging/swap.
- In long-running loops, periodically call `watchdog_kick("...")` where stalls are possible.

## Commenting and Invariant Documentation

Comments are part of the kernel specification. Keep them precise and synchronized with behavior.

### Required Invariant Tags
Use these tags on relevant paths:
- `SECURITY-INVARIANT:` for privilege/resource/security boundaries.
- `ABI-INVARIANT:` for user-visible or binary-compatibility contracts.
- `FS-INVARIANT:` for on-disk filesystem structures and compatibility rules.
- `CAPABILITY-REQUIRED:` for capability enforcement/permission gates.

### Sensitive Constants Must Be Documented
Any architecture/security/ABI/disk-format relevant constant must include:
- Why this value exists.
- Which invariant it preserves.
- What breaks if it changes.
- Whether it is ABI-sensitive.
- Whether it is disk-format-sensitive.
- Whether it is security-critical.

### Translation-Grade Comments
For bitfields, masks, flags, and on-disk formats, document:
- Bit layout and semantic meaning.
- Endianness assumptions.
- Alignment assumptions.
- Versioning and backward-compatibility constraints.

### Comment Quality Rules
- Explain why, constraints, and failure semantics, not obvious mechanics.
- Remove or update comments immediately when behavior changes.
- Do not use vague comments such as "magic number", "temporary fix", or "just works".

## Testing Expectations

Before opening a PR:
1. Build with `make build`.
2. Boot test with `make run` (or `make qemu-debug` when debugging).
3. Run focused manual tests for changed behavior.
4. If you touched user commands, rebuild binaries and regenerate docs (`make userland`, `make eynfsimg`, `make docs`).
5. If practical, run `make analyze` for extra static checks.

## Documentation Expectations

When behavior changes, update docs in the same PR.

- Command changes: regenerate `docs/command-reference.md` and `docs/help-text.txt` via `make docs`.
- API/ABI/syscall changes: update relevant files under `docs/api/`.
- Memory/paging/boot behavior changes: update `docs/general/low-memory.md` and related docs.

## Pull Request Checklist

- Scope is small and focused.
- New/changed invariants are documented in code comments.
- No stale debug output remains.
- Build and runtime sanity checks were performed.
- Documentation is updated in the same PR.

Thanks for contributing to EYN-OS.
