#ifndef IRQ_H
#define IRQ_H

#include <types.h>

// initialize PIC, remap IRQs to 0x20-0x2F, and install IDT gates
void irq_init(void);

// register a simple IRQ handler callback for IRQ line 0-15
typedef void (*irq_handler_t)(void);
void register_interrupt_handler(int irq, irq_handler_t handler);

// send end-of-interrupt to PIC
void pic_send_eoi(int irq);

// C-level IRQ dispatcher used by assembly stubs.
void irq_dispatch_c(int irq_number);

#endif // IRQ_H


