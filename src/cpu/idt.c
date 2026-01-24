#include <misc/types.h>
#include <idt.h>

// Single definitions of IDT structures (was incorrectly in header)
idt_gate_t idt[IDT_ENTRIES];
idt_register_t idt_reg;

void set_idt_gate(int n, uint32 handler) {
    idt[n].low_offset = low_16(handler);
    idt[n].sel = KERNEL_CS;
    idt[n].always0 = 0;
    idt[n].flags = 0x8E; 
    idt[n].high_offset = high_16(handler);
}

// Special function for syscall gates that need to be accessible from user mode (DPL=3)
void set_syscall_gate(int n, uint32 handler) {
    idt[n].low_offset = low_16(handler);
    idt[n].sel = KERNEL_CS;
    idt[n].always0 = 0;
    // Use a TRAP gate (type=15) so IF is not cleared on entry.
    // Otherwise, if a syscall blocks (e.g., line input), PIT IRQ0 can't run and the UI appears frozen.
    idt[n].flags = 0xEF; // 0xEF = Present(1) + DPL(3) + Type(15) = user-accessible trap gate
    idt[n].high_offset = high_16(handler);
}

void set_idt() {
    idt_reg.base = (uint32) &idt;
    idt_reg.limit = IDT_ENTRIES * sizeof(idt_gate_t) - 1;
    __asm__ __volatile__("lidtl (%0)" : : "r" (&idt_reg));
}
