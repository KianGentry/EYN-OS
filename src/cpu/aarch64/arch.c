#include <arch.h>

/*
 * AArch64 implementation of the Architecture Abstraction Layer.
 *
 * This file is not used by the current i386 build, but provides the exact same
 * exported symbols so higher-level code can remain architecture-neutral.
 */

void arch_disable_interrupts(void) {
    /* Disable IRQs (set PSTATE.I). */
    asm volatile("msr daifset, #2" ::: "memory");
}

void arch_enable_interrupts(void) {
    /* Enable IRQs (clear PSTATE.I). */
    asm volatile("msr daifclr, #2" ::: "memory");
}

arch_irq_state_t arch_irq_save(void) {
    arch_irq_state_t state;

    /* Read current DAIF, then disable IRQs. */
    asm volatile(
        "mrs %0, daif\n"
        "msr daifset, #2\n"
        : "=r"(state)
        :
        : "memory");

    return state;
}

void arch_irq_restore(arch_irq_state_t state) {
    /* Restore DAIF (IRQ mask state). */
    asm volatile("msr daif, %0" :: "r"(state) : "memory");
}

void arch_halt(void) {
    /* Wait For Interrupt */
    asm volatile("wfi" ::: "memory");
}

void arch_relax(void) {
    /* Hint to the CPU we're in a spin-wait. */
    asm volatile("yield" ::: "memory");
}

void arch_halt_forever(void) {
    arch_disable_interrupts();
    for (;;) {
        arch_halt();
    }
}
