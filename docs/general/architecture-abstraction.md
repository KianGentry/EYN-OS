# Architecture Abstraction Layer (AAL)

EYN-OS started as an i386 Multiboot kernel. To support additional CPU architectures (notably AArch64 for Raspberry Pi 4), the kernel uses a small Architecture Abstraction Layer (AAL): a stable set of functions for CPU/ISA-dependent operations.

## Goals

- Keep higher-level kernel code free of raw inline assembly.
- Reduce preprocessor sprawl (`#ifdef`) by centralizing ISA differences.
- Make it straightforward to add an AArch64 backend without rewriting unrelated subsystems.

## Current API

The initial API is intentionally small and focused on universal CPU primitives:

- Interrupt control
  - `arch_disable_interrupts()`
  - `arch_enable_interrupts()`
  - `arch_irq_save()` / `arch_irq_restore()`
- Idle / stop
  - `arch_halt()`
  - `arch_relax()`
  - `arch_halt_forever()`

Header: include/cpu/arch.h

## Implementations

- i386 implementation: src/cpu/i386/arch.c
- AArch64 implementation: src/cpu/aarch64/arch.c

The build selects the implementation using the `ARCH` Makefile variable (default: `i386`).

## Coding rules

- Higher-level code must not embed ISA-specific instructions like `cli`, `sti`, `hlt`, or `pause` directly.
- New code that needs these behaviors should call the AAL functions instead.
