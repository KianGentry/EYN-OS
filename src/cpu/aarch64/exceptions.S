// AArch64 exception vector table + minimal IRQ entry glue.
// This is intentionally small for bring-up.

    .section .text.vectors, "ax"
    .align 11
    .global aarch64_vectors
    .type aarch64_vectors, %function

// Vector table layout (AArch64): 16 entries, 0x80 bytes each.
// We only actively handle IRQ at current EL using SP_EL1.

aarch64_vectors:
    // 0x000: Synchronous EL1t
    b   aarch64_sync
    .balign 0x80
    // 0x080: IRQ EL1t
    b   aarch64_irq
    .balign 0x80
    // 0x100: FIQ EL1t
    b   aarch64_fiq
    .balign 0x80
    // 0x180: SError EL1t
    b   aarch64_serror
    .balign 0x80

    // 0x200: Synchronous EL1h
    b   aarch64_sync
    .balign 0x80
    // 0x280: IRQ EL1h
    b   aarch64_irq
    .balign 0x80
    // 0x300: FIQ EL1h
    b   aarch64_fiq
    .balign 0x80
    // 0x380: SError EL1h
    b   aarch64_serror
    .balign 0x80

    // 0x400: Synchronous EL0_64
    b   aarch64_sync
    .balign 0x80
    // 0x480: IRQ EL0_64
    b   aarch64_irq
    .balign 0x80
    // 0x500: FIQ EL0_64
    b   aarch64_fiq
    .balign 0x80
    // 0x580: SError EL0_64
    b   aarch64_serror
    .balign 0x80

    // 0x600: Synchronous EL0_32
    b   aarch64_sync
    .balign 0x80
    // 0x680: IRQ EL0_32
    b   aarch64_irq
    .balign 0x80
    // 0x700: FIQ EL0_32
    b   aarch64_fiq
    .balign 0x80
    // 0x780: SError EL0_32
    b   aarch64_serror

    // Pad to full vector table size (16 * 0x80 = 0x800)
    .balign 0x80

aarch64_vectors_end:
    .size aarch64_vectors, aarch64_vectors_end - aarch64_vectors

// --- Handlers ---

    .text

// Save a minimal set of registers, call C handler, restore.
// For bring-up we save x0-x3 + LR only.

aarch64_irq:
    sub sp, sp, #0x40
    stp x0, x1, [sp, #0x00]
    stp x2, x3, [sp, #0x10]
    stp x30, xzr, [sp, #0x20]

    bl  aarch64_irq_dispatch

    ldp x30, xzr, [sp, #0x20]
    ldp x2, x3, [sp, #0x10]
    ldp x0, x1, [sp, #0x00]
    add sp, sp, #0x40
    eret

// For now, other exceptions just hang.

aarch64_sync:
    b aarch64_panic_hang

aarch64_fiq:
    b aarch64_panic_hang

aarch64_serror:
    b aarch64_panic_hang

// Simple hang loop.
// If you want richer debug, we can later read ESR_EL1/FAR_EL1 and print.

aarch64_panic_hang:
1:
    wfi
    b 1b
