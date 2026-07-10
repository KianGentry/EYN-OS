#ifndef IDT_H
#define IDT_H

#include <misc/types.h>

#define KERNEL_CS 0x08

#if defined(EYNOS_ARCH_AMD64)
typedef struct {
    uint16 offset_low;
    uint16 sel;
    uint8 ist;
    uint8 flags;
    uint16 offset_mid;
    uint32 offset_high;
    uint32 reserved;
} __attribute__((packed)) idt_gate_t ;

typedef struct {
    uint16 limit;
    uint64 base;
} __attribute__((packed)) idt_register_t;
#else
typedef struct {
    uint16 low_offset;
    uint16 sel;
    uint8 always0;
    uint8 flags;
    uint16 high_offset;
} __attribute__((packed)) idt_gate_t ;

typedef struct {
    uint16 limit;
    uint32 base;
} __attribute__((packed)) idt_register_t;
#endif

#define IDT_ENTRIES 256
extern idt_gate_t idt[IDT_ENTRIES];
extern idt_register_t idt_reg;


/* Functions implemented in idt.c */
extern void set_idt_gate(int n, uintptr handler);
extern void set_syscall_gate(int n, uintptr handler);
extern void set_idt();

#endif