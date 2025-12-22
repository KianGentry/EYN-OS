#include <isr.h>
#include <vga.h>
#include <idt.h>
#include <multiboot.h>
#include <shell.h>
#include <util.h>
#include <string.h>
#include <kb.h>
#include <panic.h>

#include <sched.h>

#include <tile_manager.h>
#include <terminals.h>
#include <mm/user_access.h>
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
static void generic_isr_handler(regs_t* regs) {
    error_context_t ctx;
    int isr_num = (int)regs->int_no;
    ctx.error_code = isr_num;
    ctx.eip = regs->eip;
    ctx.eflags = regs->eflags;
    ctx.esp = ((regs->cs & 3) == 3) ? regs->useresp : 0;
    
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
            printf("%c[FATAL] Critical error - invoking kernel panic\n", 255, 0, 0);
            printf("%cError count: %d\n", 255, 255, 255, system_error_count);
            {
                char msg[128];
                snprintf(msg, sizeof(msg), "ISR %d at 0x%X", isr_num, ctx->eip);
                PANIC(msg);
            }
            break; // PANIC does not return
            
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

        default:
            printf("%c[WARNING] Unknown error severity - continuing\n", 255, 255, 0);
            break;
    }
}

// Individual ISR handlers - now use intelligent recovery
void isr_dispatch(regs_t* regs) {
    if (!regs) {
        return;
    }

    /* Page faults should be handled by the VMM (demand paging / COW / swap). */
    if (regs->int_no == 14) {
        extern void page_fault_handler(regs_t* r);
        page_fault_handler(regs);
        return;
    }

    generic_isr_handler(regs);
}

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
#define SYSCALL_GETKEY 6

// When the tiling manager is active, user-mode programs should print into the
// focused virtual terminal so output is visible in the graphical shell.
static void syscall_console_write(const char* buf, int len) {
    if (!buf || len <= 0) return;
    if (tile_is_tiling_active()) {
        int term = tile_get_focused();
        if (g_user_task_active) {
            term = g_user_task_term;
        }
        if (term < 0) term = 0;
        for (int i = 0; i < len; ++i) {
            vterm_write_char(term, buf[i]);
        }
        if (g_user_task_active) {
            g_user_task_ui_dirty = 1;
        }
        return;
    }
    // Fallback: use kernel printf (serial/text console)
    int pos = 0;
    while (pos < len) {
        int chunk = len - pos;
        if (chunk > 120) chunk = 120;
        char out[121];
        memcpy(out, buf + pos, chunk);
        out[chunk] = '\0';
        printf("%s", out);
        pos += chunk;
    }
}

static void syscall_maybe_render_ui(void) {
    // Intentionally left as a no-op.
    // Rendering from inside the syscall handler is fragile (can be re-entrant
    // w/ IRQ0 and does heavy GUI work while in an interrupt gate). We instead
    // repaint from the PIT IRQ0 path while a user task is active.
}

// Upper bound to keep syscall I/O bounded (protects kernel stack/time).
#define SYSCALL_IO_MAX (64 * 1024)

// C dispatcher called by the assembly stub. Returns value in EAX to user.
uint32 syscall_dispatch(regs_t* regs) {
    uint32 syscall_num = regs->eax;
    uint32 arg1 = regs->ebx;
    uint32 arg2 = regs->ecx;
    uint32 arg3 = regs->edx;

    switch (syscall_num) {
        case SYSCALL_WRITE: {
            if (arg1 == 1) {
                const char* user_buf = (const char*)arg2;
                int len = (int)arg3;
                if (len < 0) {
                    regs->eax = (uint32)-1;
                    break;
                }
                if (len == 0) {
                    regs->eax = 0;
                    break;
                }

                if (len > SYSCALL_IO_MAX) {
                    len = SYSCALL_IO_MAX;
                }

                // Copy user memory in small chunks to avoid faults/overruns.
                int pos = 0;
                while (pos < len) {
                    int chunk = len - pos;
                    if (chunk > 256) chunk = 256;
                    char tmp[256];
                    if (copyin(tmp, user_buf + pos, (size_t)chunk) != 0) {
                        regs->eax = (uint32)-1;
                        return regs->eax;
                    }
                    syscall_console_write(tmp, chunk);
                    pos += chunk;
                }

                syscall_maybe_render_ui();

                regs->eax = (uint32)len; // return bytes written (possibly clamped)
            } else {
                regs->eax = (uint32)-1;
            }
            break;
        }
        case SYSCALL_READ: {
            // read(fd=0, buf=arg2, len=arg3)
            if (arg3 == 0) { regs->eax = 0; break; }
            if (arg1 != 0 || arg2 == 0) { regs->eax = (uint32)-1; break; }
            // Use keyboard driver to read a line (echoed by driver); returns malloc'd buffer
            string s = readStr();
            if (!s) { regs->eax = (uint32)-1; break; }
            int slen = (int)strlen(s);
            int maxcpy = (int)arg3;
            if (maxcpy <= 0) { free(s); regs->eax = 0; break; }
            // Copy up to len-1 bytes and NUL-terminate for convenience
            int n = slen;
            if (n > maxcpy - 1) n = maxcpy - 1;
            if (n < 0) n = 0;

            char* user_dst = (char*)arg2;
            // Ensure user buffer is writable for the bytes we will touch.
            if (copyout(user_dst, s, (size_t)n) != 0) {
                free(s);
                regs->eax = (uint32)-1;
                break;
            }
            // Write NUL terminator if possible.
            if (copyout(user_dst + n, "\0", 1) != 0) {
                free(s);
                regs->eax = (uint32)-1;
                break;
            }

            regs->eax = (uint32)n;
            free(s);
            break;
        }
        case SYSCALL_EXIT: {
            // Keep this visible in the graphical shell too.
            char msg[64];
            int n = snprintf(msg, sizeof(msg), "[SYSCALL] Program exited with code %d\n", (int)arg1);
            if (n > 0) syscall_console_write(msg, n);
            g_user_task_active = 0;
            g_user_task_term = -1;
            g_user_interrupt = 0;
            g_abort_to_shell = 1;
            regs->eax = 0;
            break;
        }
        case SYSCALL_GETKEY: {
            regs->eax = (uint32)kb_getchar_nonblocking();
            break;
        }
        default: {
            printf("%c[SYSCALL] Unknown syscall: %d\n", 255, 0, 0, syscall_num);
            regs->eax = (uint32)-1;
            break;
        }
    }

    return regs->eax;
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

