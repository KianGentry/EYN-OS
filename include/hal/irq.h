#ifndef EYNOS_HAL_IRQ_H
#define EYNOS_HAL_IRQ_H

#include <misc/types.h>

/*
 * HAL IRQ
 *
 * Purpose:
 *  Provide a uniform interrupt registration surface. The backend translates
 *  platform interrupt controller details (PIC/APIC vs GIC) into logical IRQ IDs.
 *
 * Contract:
 *  - Handlers must be fast and must not block.
 *  - Backend is responsible for EOI/ack as required by the controller.
 */

typedef void (*hal_irq_handler_t)(uint32 irq_id, void* user);

/* Register a handler for a logical IRQ ID. Returns 0 on success. */
int hal_irq_register(uint32 irq_id, hal_irq_handler_t handler, void* user);

/* Enable/disable a logical IRQ ID. */
void hal_irq_enable(uint32 irq_id);
void hal_irq_disable(uint32 irq_id);

#endif
