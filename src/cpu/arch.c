#include <arch.h>

void arch_disable_interrupts(void) {
    asm volatile("cli" ::: "memory");
}

void arch_enable_interrupts(void) {
    asm volatile("sti" ::: "memory");
}

arch_irq_state_t arch_irq_save(void) {
    arch_irq_state_t flags;
    asm volatile(
        "pushf\n"
        "pop %0\n"
        "cli\n"
        : "=r"(flags)
        :
        : "memory", "cc");
    return flags;
}

void arch_irq_restore(arch_irq_state_t state) {
    asm volatile(
        "push %0\n"
        "popf\n"
        :
        : "r"(state)
        : "memory", "cc");
}

void arch_halt(void) {
    asm volatile("hlt" ::: "memory");
}

void arch_relax(void) {
    /* pause is a hint on x86; safe as a tight-loop relax primitive */
    asm volatile("pause" ::: "memory");
}

void arch_halt_forever(void) {
    arch_disable_interrupts();
    for (;;) {
        arch_halt();
    }
}
