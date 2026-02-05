#include <hal/irq.h>

#include <cpu/aarch64/irq_router.h>

int hal_irq_register(uint32 irq_id, hal_irq_handler_t handler, void* user) {
    /* Route directly to the CPU router. */
    return aarch64_irq_register_handler(irq_id, (aarch64_irq_handler_t)handler, user);
}

void hal_irq_enable(uint32 irq_id) {
    aarch64_irq_enable_id(irq_id);
}

void hal_irq_disable(uint32 irq_id) {
    aarch64_irq_disable_id(irq_id);
}
