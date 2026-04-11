#ifndef IRQ_H
#define IRQ_H

#include <misc/types.h>

// initialize PIC, remap IRQs to 0x20-0x2F, and install IDT gates
void irq_init(void);

// register a simple IRQ handler callback for IRQ line 0-15
typedef void (*irq_handler_t)(void);
void register_interrupt_handler(int irq, irq_handler_t handler);

// send end-of-interrupt to PIC
void pic_send_eoi(int irq);

// C-level IRQ dispatcher used by assembly stubs.
// frame_ptr points at the saved GPR block produced by the IRQ entry stub.
void irq_dispatch_c(int irq_number, uintptr frame_ptr);

// Deferred IRQ dispatch for deterministic mode (no PIC EOI).
void irq_dispatch_deferred(int irq_number);

#endif // IRQ_H


