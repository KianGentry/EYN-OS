#include <isr.h>
#include <vga.h>
#include <idt.h>
#include <multiboot.h>
#include <shell.h>
#include <util.h>
#include <string.h>

extern multiboot_info_t *g_mbi;

// Global error tracking
static volatile int system_error_count = 0;
static volatile int last_error_code = 0;
static volatile uint32 last_error_eip = 0;

// Error severity levels
#define ERROR_FATAL 0
#define ERROR_RECOVERABLE 1
#define ERROR_WARNING 2

// Error context structure
typedef struct {
    int error_code;
    uint32 eip;
    uint32 eflags;
    uint32 esp;
    int severity;
} error_context_t;

// Forward declarations
static void handle_error(int isr_num, error_context_t* ctx);
static int is_recoverable_error(int isr_num);
static void log_error(int isr_num, error_context_t* ctx);
static void attempt_recovery(error_context_t* ctx);

extern void isr_install() 
{
    set_idt_gate(0, (uint32)isr0);
    set_idt_gate(1, (uint32)isr1);
    set_idt_gate(2, (uint32)isr2);
    set_idt_gate(3, (uint32)isr3);
    set_idt_gate(4, (uint32)isr4);
    set_idt_gate(5, (uint32)isr5);
    set_idt_gate(6, (uint32)isr6);
    set_idt_gate(7, (uint32)isr7);
    set_idt_gate(8, (uint32)isr8);
    set_idt_gate(9, (uint32)isr9);
    set_idt_gate(10, (uint32)isr10);
    set_idt_gate(11, (uint32)isr11);
    set_idt_gate(12, (uint32)isr12);
    set_idt_gate(13, (uint32)isr13);
    set_idt_gate(14, (uint32)isr14);
    set_idt_gate(15, (uint32)isr15);
    set_idt_gate(16, (uint32)isr16);
    set_idt_gate(17, (uint32)isr17);
    set_idt_gate(18, (uint32)isr18);
    set_idt_gate(19, (uint32)isr19);
    set_idt_gate(20, (uint32)isr20);
    set_idt_gate(21, (uint32)isr21);
    set_idt_gate(22, (uint32)isr22);
    set_idt_gate(23, (uint32)isr23);
    set_idt_gate(24, (uint32)isr24);
    set_idt_gate(25, (uint32)isr25);
    set_idt_gate(26, (uint32)isr26);
    set_idt_gate(27, (uint32)isr27);
    set_idt_gate(28, (uint32)isr28);
    set_idt_gate(29, (uint32)isr29);
    set_idt_gate(30, (uint32)isr30);
    set_idt_gate(31, (uint32)isr31);
    
    // Set up syscall handler (interrupt 0x80) to the assembly stub
    extern void syscall_entry();
    set_syscall_gate(0x80, (uint32)syscall_entry);

    set_idt(); // Load with ASM
}

// Generic ISR handler that captures context and attempts recovery
static void generic_isr_handler(int isr_num) {
    error_context_t ctx;
    ctx.error_code = isr_num;
    ctx.eip = 0; // Will be set by assembly wrapper
    ctx.eflags = 0;
    ctx.esp = 0;
    
    // Determine error severity
    if (is_recoverable_error(isr_num)) {
        ctx.severity = ERROR_RECOVERABLE;
    } else if (isr_num == 0 || isr_num == 6 || isr_num == 8 || isr_num == 13 || isr_num == 14) {
        ctx.severity = ERROR_FATAL;
    } else {
        ctx.severity = ERROR_WARNING;
    }
    
    // Log the error
    log_error(isr_num, &ctx);
    
    // Handle based on severity
    handle_error(isr_num, &ctx);
}

// Determine if an error is potentially recoverable
static int is_recoverable_error(int isr_num) {
    // Most errors are recoverable except critical ones
    switch (isr_num) {
        case 0:  // Division by zero - can be recovered
        case 1:  // Debug - recoverable
        case 3:  // Breakpoint - recoverable
        case 4:  // Overflow - recoverable
        case 5:  // Bounds - recoverable
        case 7:  // No coprocessor - recoverable
        case 9:  // Coprocessor segment overrun - recoverable
        case 10: // Bad TSS - recoverable
        case 11: // Segment not present - recoverable
        case 12: // Stack fault - recoverable
        case 15: // Unknown interrupt - recoverable
        case 16: // Coprocessor fault - recoverable
        case 17: // Alignment check - recoverable
        case 18: // Machine check - recoverable
            return 1;
        case 6:  // Invalid opcode - potentially fatal
        case 8:  // Double fault - fatal
        case 13: // General protection fault - potentially fatal
        case 14: // Page fault - potentially fatal
            return 0;
        default:
            return 1;
    }
}

// Log error with context
static void log_error(int isr_num, error_context_t* ctx) {
    system_error_count++;
    last_error_code = isr_num;
    last_error_eip = ctx->eip;
    
    printf("%c[ERROR] %s (ISR %d) at 0x%X\n", 255, 0, 0, 
           exception_messages[isr_num], isr_num, ctx->eip);
    
    if (ctx->severity == ERROR_FATAL) {
        printf("%c[FATAL] System may be unstable\n", 255, 0, 0);
    } else if (ctx->severity == ERROR_RECOVERABLE) {
        printf("%c[RECOVERABLE] Attempting to continue...\n", 255, 165, 0);
    } else {
        printf("%c[WARNING] Non-critical error\n", 255, 255, 0);
    }
}

// Attempt recovery based on error type
static void attempt_recovery(error_context_t* ctx) {
    switch (ctx->error_code) {
        case 0: // Division by zero
            // Set result to 0 and continue
            printf("%c[RECOVERY] Division by zero handled\n", 0, 255, 0);
            break;
        case 1: // Debug
            printf("%c[RECOVERY] Debug exception handled\n", 0, 255, 0);
            break;
        case 3: // Breakpoint
            printf("%c[RECOVERY] Breakpoint handled\n", 0, 255, 0);
            break;
        case 4: // Overflow
            printf("%c[RECOVERY] Overflow handled\n", 0, 255, 0);
            break;
        case 5: // Bounds
            printf("%c[RECOVERY] Bounds check handled\n", 0, 255, 0);
            break;
        default:
            printf("%c[RECOVERY] Generic error recovery\n", 0, 255, 0);
            break;
    }
}

// Handle error based on severity
static void handle_error(int isr_num, error_context_t* ctx) {
    switch (ctx->severity) {
        case ERROR_FATAL:
            printf("%c[FATAL] Critical error - system halt\n", 255, 0, 0);
            printf("%cError count: %d\n", 255, 255, 255, system_error_count);
            asm("hlt");
            break;
            
        case ERROR_RECOVERABLE:
            attempt_recovery(ctx);
            // Return to shell instead of halting
            printf("%c[RECOVERY] Returning to shell...\n", 0, 255, 0);
            // Clear any pending interrupts
            asm("cli");
            asm("sti");
            break;
            
        case ERROR_WARNING:
            printf("%c[WARNING] Non-critical error - continuing\n", 255, 255, 0);
            break;
    }
}

// Individual ISR handlers - now use intelligent recovery
void isr0() { generic_isr_handler(0); }
void isr1() { generic_isr_handler(1); }
void isr2() { generic_isr_handler(2); }
void isr3() { generic_isr_handler(3); }
void isr4() { generic_isr_handler(4); }
void isr5() { generic_isr_handler(5); }
void isr6() { generic_isr_handler(6); }
void isr7() { generic_isr_handler(7); }
void isr8() { generic_isr_handler(8); }
void isr9() { generic_isr_handler(9); }
void isr10() { generic_isr_handler(10); }
void isr11() { generic_isr_handler(11); }
void isr12() { generic_isr_handler(12); }
void isr13() { generic_isr_handler(13); }
void isr14() { 
    // Call our custom page fault handler
    extern void page_fault_handler(regs_t* r);
    regs_t r;
    // Extract registers from stack (simplified)
    asm volatile("mov %%esp, %0" : "=r" (r.esp));
    r.err_code = 0; // Page fault error code
    r.cs = 0x08; // Kernel code segment
    page_fault_handler(&r);
}
void isr15() { generic_isr_handler(15); }
void isr16() { generic_isr_handler(16); }
void isr17() { generic_isr_handler(17); }
void isr18() { generic_isr_handler(18); }
void isr19() { generic_isr_handler(19); }
void isr20() { generic_isr_handler(20); }
void isr21() { generic_isr_handler(21); }
void isr22() { generic_isr_handler(22); }
void isr23() { generic_isr_handler(23); }
void isr24() { generic_isr_handler(24); }
void isr25() { generic_isr_handler(25); }
void isr26() { generic_isr_handler(26); }
void isr27() { generic_isr_handler(27); }
void isr28() { generic_isr_handler(28); }
void isr29() { generic_isr_handler(29); }
void isr30() { generic_isr_handler(30); }
void isr31() { generic_isr_handler(31); }

// Error status functions for shell commands
int get_system_error_count() {
    return system_error_count;
}

int get_last_error_code() {
    return last_error_code;
}

uint32 get_last_error_eip() {
    return last_error_eip;
}

// Syscall numbers
#define SYSCALL_WRITE 1
#define SYSCALL_EXIT  2
#define SYSCALL_READ  3
#define SYSCALL_OPEN  4
#define SYSCALL_CLOSE 5

// C dispatcher called by the assembly stub. Returns value in EAX to user.
uint32 syscall_dispatch(regs_t* r) {
    uint32 syscall_num = r->eax;
    uint32 arg1 = r->ebx;
    uint32 arg2 = r->ecx;
    uint32 arg3 = r->edx;
    printf("[SYSCALL] num=%d a1=%d a2=%d a3=%d\n", (int)syscall_num, (int)arg1, (int)arg2, (int)arg3);

    switch (syscall_num) {
        case SYSCALL_WRITE: {
            if (arg1 == 1) {
                char* buffer = (char*)arg2;
                int len = (int)arg3;
                printf("[SYSCALL] write: fd=1 buf=%d len=%d\n", (int)buffer, len);
                // Print in chunks using %s to avoid the special color control for leading "%c"
                int pos = 0;
                while (pos < len) {
                    int chunk = len - pos;
                    if (chunk > 120) chunk = 120; // small on-stack buffer
                    char out[121];
                    memcpy(out, buffer + pos, chunk);
                    out[chunk] = '\0';
                    printf("%s", out);
                    pos += chunk;
                }
                r->eax = (uint32)len; // return bytes written
            } else {
                r->eax = (uint32)-1;
            }
            break;
        }
        case SYSCALL_READ: {
            r->eax = 0; // no input yet
            break;
        }
        case SYSCALL_EXIT: {
            printf("%c[SYSCALL] Program exited with code %d\n", 0, 255, 0, arg1);
            r->eax = 0;
            break;
        }
        default: {
            printf("%c[SYSCALL] Unknown syscall: %d\n", 255, 0, 0, syscall_num);
            r->eax = (uint32)-1;
            break;
        }
    }

    return r->eax;
}


/*To printf the message which defines every exception */
string exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",

    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",

    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",

    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved"
};

