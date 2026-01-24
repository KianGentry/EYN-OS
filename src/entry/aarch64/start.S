// AArch64 Raspberry Pi 4 firmware entry
// Firmware loads kernel8.img at 0x80000 and enters at _start with:
//   x0 = physical address of the device tree blob (DTB)

    .section .text._start, "ax"
    .global _start
    .type _start, %function

_start:
    // Set up a temporary stack.
    adrp x1, __stack_top
    add  x1, x1, :lo12:__stack_top
    mov  sp, x1

    // Preserve DTB pointer for C.
    mov  x19, x0

    // Zero BSS: [__bss_start, __bss_end)
    adrp x0, __bss_start
    add  x0, x0, :lo12:__bss_start
    adrp x1, __bss_end
    add  x1, x1, :lo12:__bss_end

    // x0 = current, x1 = end
1:
    cmp  x0, x1
    b.hs 2f
    str  xzr, [x0], #8
    b    1b
2:

    // Install exception vectors for EL1.
    adrp x0, aarch64_vectors
    add  x0, x0, :lo12:aarch64_vectors
    msr  vbar_el1, x0
    isb

    // Call kernel_main(dtb_ptr)
    mov  x0, x19
    bl   kernel_main

    // If kernel_main returns, halt.
3:
    wfi
    b 3b

    .size _start, . - _start
