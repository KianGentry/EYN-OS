#include <misc/types.h>
#include <arch.h>

void uart_pl011_write(const char* s);
void uart_pl011_putc(char c);
void uart_pl011_write_hex64(uint64 v);

static void uart_write_hex64(uint64 v) {
    (void)uart_pl011_putc;
    uart_pl011_write_hex64(v);
}

static inline uint64 read_sysreg_esr_el1(void) {
    uint64 v;
    asm volatile("mrs %0, esr_el1" : "=r"(v));
    return v;
}

static inline uint64 read_sysreg_far_el1(void) {
    uint64 v;
    asm volatile("mrs %0, far_el1" : "=r"(v));
    return v;
}

static inline uint64 read_sysreg_elr_el1(void) {
    uint64 v;
    asm volatile("mrs %0, elr_el1" : "=r"(v));
    return v;
}

static inline uint64 read_sysreg_spsr_el1(void) {
    uint64 v;
    asm volatile("mrs %0, spsr_el1" : "=r"(v));
    return v;
}

static inline uint64 read_sysreg_currentel(void) {
    uint64 v;
    asm volatile("mrs %0, CurrentEL" : "=r"(v));
    return v;
}

/*
 * Synchronous exception handler for early bring-up.
 *
 * Purpose:
 * - Provide immediate visibility into crashes by printing ESR/FAR/ELR/SPSR.
 * - Keep it minimal and safe for freestanding early boot.
 *
 * Side effects:
 * - Disables interrupts and halts forever after printing.
 */
void aarch64_sync_dispatch(void) {
    arch_disable_interrupts();

    uint64 esr = read_sysreg_esr_el1();
    uint64 far = read_sysreg_far_el1();
    uint64 elr = read_sysreg_elr_el1();
    uint64 spsr = read_sysreg_spsr_el1();
    uint64 cel = read_sysreg_currentel();

    uart_pl011_write("\nAArch64 SYNC EXCEPTION\n");
    uart_pl011_write("CurrentEL ");
    uart_write_hex64(cel);
    uart_pl011_write("\n");

    uart_pl011_write("ESR_EL1 ");
    uart_write_hex64(esr);
    uart_pl011_write(" FAR_EL1 ");
    uart_write_hex64(far);
    uart_pl011_write("\n");

    uart_pl011_write("ELR_EL1 ");
    uart_write_hex64(elr);
    uart_pl011_write(" SPSR_EL1 ");
    uart_write_hex64(spsr);
    uart_pl011_write("\n");

    for (;;) {
        asm volatile("wfi" ::: "memory");
    }
}
