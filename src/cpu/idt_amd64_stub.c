#include <misc/types.h>
#include <idt.h>

/*
 * ABI-INVARIANT: amd64 IDT entries are 16-byte descriptors with full
 * 64-bit handler offsets. Truncating handler pointers to 32 bits can route
 * exceptions/IRQs into unmapped memory once kernel text moves above 4 GiB.
 */
idt_gate_t idt[IDT_ENTRIES];
idt_register_t idt_reg;

static inline void idt_set_gate_common(int n, uintptr handler, uint8 flags) {
    if (n < 0 || n >= IDT_ENTRIES) {
        return;
    }

    idt[n].offset_low = (uint16)(handler & 0xFFFFu);
    idt[n].sel = KERNEL_CS;
    idt[n].ist = 0;
    idt[n].flags = flags;
    idt[n].offset_mid = (uint16)((handler >> 16) & 0xFFFFu);
    idt[n].offset_high = (uint32)((handler >> 32) & 0xFFFFFFFFu);
    idt[n].reserved = 0;
}

void set_idt_gate(int n, uintptr handler) {
    idt_set_gate_common(n, handler, 0x8E);
}

void set_syscall_gate(int n, uintptr handler) {
    idt_set_gate_common(n, handler, 0xEF);
}

void set_idt(void) {
    idt_reg.base = (uint64)(uintptr)&idt[0];
    idt_reg.limit = (uint16)(IDT_ENTRIES * sizeof(idt_gate_t) - 1u);
    asm volatile("lidt %0" : : "m"(idt_reg));
}
