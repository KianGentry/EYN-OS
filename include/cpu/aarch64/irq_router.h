#ifndef EYNOS_CPU_AARCH64_IRQ_ROUTER_H
#define EYNOS_CPU_AARCH64_IRQ_ROUTER_H

#include <misc/types.h>

/*
 * AArch64 IRQ router surface.
 *
 * Purpose:
 *  Provide a small, stable API to register and control IRQ handlers that are
 *  ultimately dispatched from the GICv2 IRQ path.
 *
 * Notes:
 *  - IRQ IDs are GIC logical IDs (0..1023). Timer IRQ ID is discovered from DTB
 *    and wired during platform init.
 */

typedef void (*aarch64_irq_handler_t)(uint32 irq_id, void* user);
typedef void (*aarch64_tick_handler_t)(void* user);

int aarch64_irq_register_handler(uint32 irq_id, aarch64_irq_handler_t handler, void* user);
void aarch64_irq_enable_id(uint32 irq_id);
void aarch64_irq_disable_id(uint32 irq_id);

/* Timer tick callback: called from the timer IRQ fast path after rearming. */
void aarch64_irq_set_tick_handler(aarch64_tick_handler_t handler, void* user);

#endif
