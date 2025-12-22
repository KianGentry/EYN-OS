#include <types.h>
#include "multiboot.h"

#ifndef ISR_H
#define ISR_H

/* ISRs reserved for CPU exceptions */
void isr0();
void isr1();
void isr2();
void isr3();
void isr4();
void isr5();
void isr6();
void isr7();
void isr8();
void isr9();
void isr10();
void isr11();
void isr12();
void isr13();
void isr14();
void isr15();
void isr16();
void isr17();
void isr18();
void isr19();
void isr20();
void isr21();
void isr22();
void isr23();
void isr24();
void isr25();
void isr26();
void isr27();
void isr28();
void isr29();
void isr30();
void isr31();

extern string exception_messages[32];

extern void isr_install();

// Error tracking functions
int get_system_error_count();
int get_last_error_code();
uint32 get_last_error_eip();

// Register state as laid out by syscall ISR stub (starting at saved EDI)
typedef struct regs_t {
    uint32 edi;
    uint32 esi;
    uint32 ebp;
    uint32 esp;      // value prior to pusha (not current ESP)
    uint32 ebx;
    uint32 edx;
    uint32 ecx;
    uint32 eax;
    uint32 int_no;
    uint32 err_code;
    uint32 eip;
    uint32 cs;
    uint32 eflags;
    uint32 useresp;  // present only if switching rings
    uint32 ss;       // present only if switching rings
} regs_t;

// Syscall C dispatcher
uint32 syscall_dispatch(regs_t* r);

// Reset the user-task file descriptor table (used when starting/ending ring3 tasks).
void syscall_reset_user_fds(void);

// CPU exception C dispatcher (called from src/cpu/isr.asm)
void isr_dispatch(regs_t* regs);

#endif
