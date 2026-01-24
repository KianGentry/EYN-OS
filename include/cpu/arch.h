#ifndef EYNOS_CPU_ARCH_H
#define EYNOS_CPU_ARCH_H

/*
 * Architecture Abstraction Layer (AAL)
 *
 * Purpose:
 *  Provide a tiny, stable interface for CPU/ISA-dependent operations so higher-level
 *  kernel code (panic paths, scheduler idle loops, etc.) does not contain raw
 *  inline assembly or x86-specific instructions.
 *
 * Notes:
 *  - Current implementation targets i386 (see src/cpu/arch.c).
 *  - The Raspberry Pi 4 port will provide an AArch64 implementation with the same API.
 */

#include <misc/types.h>

typedef uint32 arch_irq_state_t;

/* Interrupt state management */
void arch_disable_interrupts(void);
void arch_enable_interrupts(void);
arch_irq_state_t arch_irq_save(void);
void arch_irq_restore(arch_irq_state_t state);

/* CPU idle / halt helpers */
void arch_halt(void);
void arch_relax(void);

/* Fatal stop: disables interrupts and halts forever. */
__attribute__((noreturn)) void arch_halt_forever(void);

#endif
