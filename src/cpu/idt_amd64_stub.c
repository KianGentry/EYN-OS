#include <misc/types.h>
#include <idt.h>

/*
 * Milestone A amd64 scaffold:
 * Keep IDT symbols linkable while amd64 descriptor format work is in progress.
 */
idt_gate_t idt[IDT_ENTRIES];
idt_register_t idt_reg;

void set_idt_gate(int n, uint32 handler) {
    idt[n].low_offset = low_16(handler);
    idt[n].sel = KERNEL_CS;
    idt[n].always0 = 0;
    idt[n].flags = 0x8E;
    idt[n].high_offset = high_16(handler);
}

void set_syscall_gate(int n, uint32 handler) {
    idt[n].low_offset = low_16(handler);
    idt[n].sel = KERNEL_CS;
    idt[n].always0 = 0;
    idt[n].flags = 0xEF;
    idt[n].high_offset = high_16(handler);
}

void set_idt(void) {
    idt_reg.base = 0u;
    idt_reg.limit = 0u;
}
