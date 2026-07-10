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
        "pushfq\n"
        "popq %0\n"
        "cli\n"
        : "=r"(flags)
        :
        : "memory", "cc");
    return flags;
}

void arch_irq_restore(arch_irq_state_t state) {
    asm volatile(
        "pushq %0\n"
        "popfq\n"
        :
        : "r"(state)
        : "memory", "cc");
}

void arch_halt(void) {
    asm volatile("hlt" ::: "memory");
}

void arch_relax(void) {
    asm volatile("pause" ::: "memory");
}

void arch_halt_forever(void) {
    arch_disable_interrupts();
    for (;;) {
        arch_halt();
    }
}
